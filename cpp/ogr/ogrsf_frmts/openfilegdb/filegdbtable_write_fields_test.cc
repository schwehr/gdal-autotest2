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
// Tests OpenFileGDB table field writing, rewriting, deletion and alteration
// (filegdbtable_write_fields.cpp).

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/filegdbtable_write_fields_test";

class FileGDBTableWriteFieldsTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIMkdir(kTestDir, 0755); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::vector<OGRField> MakeUnsetFields(int count) {
  std::vector<OGRField> fields(count);
  for (auto& f : fields) {
    f.Set.nMarker1 = OGRUnsetMarker;
    f.Set.nMarker2 = OGRUnsetMarker;
    f.Set.nMarker3 = OGRUnsetMarker;
  }
  return fields;
}

TEST_F(FileGDBTableWriteFieldsTest, CreateFieldValidationAndErrors) {
  const std::string table_path = std::string(kTestDir) + "/errors.gdbtable";

  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_POINT,
                            false, false));

  // 1. Unhandled field type (FGFT_RASTER)
  {
    auto raster_field = std::make_unique<OpenFileGDB::FileGDBField>(
        "raster", "", OpenFileGDB::FGFT_RASTER, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateField(std::move(raster_field)));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Unhandled field type"));
  }

  // 2. Add valid field
  auto oid_field = std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table->CreateField(std::move(oid_field)));

  // 3. Second ObjectId field rejected
  {
    auto oid2 = std::make_unique<OpenFileGDB::FileGDBField>(
        "OID2", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateField(std::move(oid2)));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Only one ObjectId field supported"));
  }

  // 4. Duplicate field name rejected
  {
    auto dup = std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateField(std::move(dup)));
    EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("already exists"));
  }

  // 5. Add geometry field
  std::vector<double> grid_res = {10.0};
  auto geom_field = std::make_unique<OpenFileGDB::FileGDBGeomField>(
      "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid_res);
  ASSERT_TRUE(table->CreateField(std::move(geom_field)));

  // 6. Second geometry field rejected
  {
    auto geom2 = std::make_unique<OpenFileGDB::FileGDBGeomField>(
        "Shape2", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid_res);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateField(std::move(geom2)));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Only one geometry field supported"));
  }

  // Sync table and insert a feature to make it non-empty
  ASSERT_TRUE(table->Sync());
  auto flds = MakeUnsetFields(table->GetFieldCount());
  OGRPoint pt(1.0, 2.0);
  ASSERT_TRUE(table->CreateFeature(flds, &pt));
  ASSERT_EQ(1, table->GetTotalRecordCount());

  // 7. Non-empty table errors:
  // Cannot add geometry field to non-empty table
  {
    // Need table with no geom field first
    const std::string no_geom_path = std::string(kTestDir) + "/nogeom.gdbtable";
    auto ng_table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(ng_table->Create(no_geom_path.c_str(), 4,
                                 OpenFileGDB::FGTGT_NONE, false, false));
    auto oid = std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    ASSERT_TRUE(ng_table->CreateField(std::move(oid)));
    ASSERT_TRUE(ng_table->Sync());
    auto nflds = MakeUnsetFields(ng_table->GetFieldCount());
    ASSERT_TRUE(ng_table->CreateFeature(nflds, nullptr));
    ASSERT_TRUE(ng_table->Sync());

    auto geom_new = std::make_unique<OpenFileGDB::FileGDBGeomField>(
        "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid_res);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ng_table->CreateField(std::move(geom_new)));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        testing::HasSubstr("Cannot add a geometry field to a non-empty table"));
  }

  // 8. Add ObjectId to non-empty table that had no ObjectId field
  {
    const std::string no_oid_path = std::string(kTestDir) + "/nooid.gdbtable";
    auto noid_table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(noid_table->Create(no_oid_path.c_str(), 4,
                                   OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(
        noid_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
            "fld1", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
            OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(noid_table->Sync());
    auto noid_flds = MakeUnsetFields(noid_table->GetFieldCount());
    ASSERT_TRUE(noid_table->CreateFeature(noid_flds, nullptr));
    ASSERT_TRUE(noid_table->Sync());

    auto oid_new = std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    EXPECT_TRUE(noid_table->CreateField(std::move(oid_new)));
    ASSERT_TRUE(noid_table->Sync());
  }

  // 9. Cannot add non-nullable field without default value to a non-empty table
  {
    auto non_null_no_def = std::make_unique<OpenFileGDB::FileGDBField>(
        "req_fld", "", OpenFileGDB::FGFT_INT32, false, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateField(std::move(non_null_no_def)));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr(
                    "Cannot add non-nullable field without default value"));
  }

  // 9. Read-only table cannot create fields
  table->Close();
  auto ro_table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(ro_table->Open(table_path.c_str(), /*bUpdate=*/false));
  {
    auto any_field = std::make_unique<OpenFileGDB::FileGDBField>(
        "fld", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    EXPECT_FALSE(ro_table->CreateField(std::move(any_field)));
  }
}

TEST_F(FileGDBTableWriteFieldsTest, AllFieldTypesAndDefaultsEmptyAndUTF16) {
  // 1. Table with UTF-8, Z and M geometry, and all field types with defaults
  const std::string table_path =
      std::string(kTestDir) + "/all_fields_utf8.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_POINT,
                            /*bGeomTypeHasZ=*/true, /*bGeomTypeHasM=*/true));

  auto oid = std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table->CreateField(std::move(oid)));

  std::vector<double> grid_res = {10.0, 50.0};
  auto geom = std::make_unique<OpenFileGDB::FileGDBGeomField>(
      "Shape", "GeomAlias", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid_res);
  geom->SetZOriginScaleTolerance(0.0, 1000.0, 0.001);
  geom->SetMOriginScaleTolerance(0.0, 1000.0, 0.001);
  geom->SetXYMinMax(-180.0, -90.0, 180.0, 90.0);
  geom->SetZMinMax(0.0, 1000.0);
  geom->SetMMinMax(0.0, 500.0);
  ASSERT_TRUE(table->CreateField(std::move(geom)));

  // Add fields with non-empty defaults
  OGRField def_int16;
  def_int16.Integer = 123;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_int16", "alias", OpenFileGDB::FGFT_INT16, true, false, true, 0,
      def_int16)));

  OGRField def_int32;
  def_int32.Integer = 123456;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_int32", "alias", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      def_int32)));

  OGRField def_f32;
  def_f32.Real = 1.25;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_float32", "alias", OpenFileGDB::FGFT_FLOAT32, true, false, true, 0,
      def_f32)));

  OGRField def_f64;
  def_f64.Real = 3.14159265;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_float64", "alias", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      def_f64)));

  char str_val[] = "hello_world";
  OGRField def_str;
  def_str.String = str_val;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_str", "alias", OpenFileGDB::FGFT_STRING, true, false, true, 50,
      def_str)));

  OGRField def_dt;
  def_dt.Date.Year = 2024;
  def_dt.Date.Month = 5;
  def_dt.Date.Day = 15;
  def_dt.Date.Hour = 10;
  def_dt.Date.Minute = 30;
  def_dt.Date.Second = 45.0f;
  def_dt.Date.TZFlag = 100;
  auto dt_field = std::make_unique<OpenFileGDB::FileGDBField>(
      "f_dt", "alias", OpenFileGDB::FGFT_DATETIME, true, false, true, 0,
      def_dt);
  dt_field->SetHighPrecision();
  ASSERT_TRUE(table->CreateField(std::move(dt_field)));

  OGRField def_date;
  def_date.Date.Year = 2024;
  def_date.Date.Month = 6;
  def_date.Date.Day = 1;
  def_date.Date.Hour = 0;
  def_date.Date.Minute = 0;
  def_date.Date.Second = 0.0f;
  def_date.Date.TZFlag = 0;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_date", "alias", OpenFileGDB::FGFT_DATE, true, false, true, 0,
      def_date)));

  OGRField def_time;
  def_time.Date.Year = 0;
  def_time.Date.Month = 0;
  def_time.Date.Day = 0;
  def_time.Date.Hour = 14;
  def_time.Date.Minute = 25;
  def_time.Date.Second = 30.0f;
  def_time.Date.TZFlag = 0;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_time", "alias", OpenFileGDB::FGFT_TIME, true, false, true, 0,
      def_time)));

  OGRField def_dto;
  def_dto.Date.Year = 2024;
  def_dto.Date.Month = 7;
  def_dto.Date.Day = 4;
  def_dto.Date.Hour = 12;
  def_dto.Date.Minute = 0;
  def_dto.Date.Second = 0.0f;
  def_dto.Date.TZFlag = 104;  // +1:00 offset
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_dto", "alias", OpenFileGDB::FGFT_DATETIME_WITH_OFFSET, true, false,
      true, 0, def_dto)));

  OGRField def_i64;
  def_i64.Integer64 = 9876543210123LL;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_int64", "alias", OpenFileGDB::FGFT_INT64, true, false, true, 0,
      def_i64)));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_bin", "alias", OpenFileGDB::FGFT_BINARY, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_guid", "alias", OpenFileGDB::FGFT_GUID, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_gid", "alias", OpenFileGDB::FGFT_GLOBALID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_xml", "alias", OpenFileGDB::FGFT_XML, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  // WriteFieldDescriptors at EOF
  ASSERT_TRUE(table->Sync());

  // Add one more field while still at EOF and Sync() again -> bUpdateFileSize =
  // true
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_extra", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->Sync());

  // 2. Table with UTF-16, TZFlag = 0 for datetime_with_offset, and without
  // defaults
  const std::string utf16_path =
      std::string(kTestDir) + "/all_fields_utf16.gdbtable";
  auto u16_table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(u16_table->Create(utf16_path.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                                false, false));
  ASSERT_TRUE(u16_table->SetTextUTF16());

  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));

  char str_utf16_def[] = "test_utf16";
  OGRField def_s_u16;
  def_s_u16.String = str_utf16_def;
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_str_u16", "", OpenFileGDB::FGFT_STRING, true, false, true, 100,
          def_s_u16)));

  OGRField def_dto_no_tz;
  def_dto_no_tz.Date.Year = 2024;
  def_dto_no_tz.Date.Month = 1;
  def_dto_no_tz.Date.Day = 1;
  def_dto_no_tz.Date.Hour = 0;
  def_dto_no_tz.Date.Minute = 0;
  def_dto_no_tz.Date.Second = 0.0f;
  def_dto_no_tz.Date.TZFlag = 0;  // TZFlag <= 1 branch
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_dto_no_tz", "", OpenFileGDB::FGFT_DATETIME_WITH_OFFSET, true,
          false, true, 0, def_dto_no_tz)));

  // Add fields with UNSET_FIELD defaults
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_i16_nodef", "", OpenFileGDB::FGFT_INT16, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_i32_nodef", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_f32_nodef", "", OpenFileGDB::FGFT_FLOAT32, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_f64_nodef", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_str_nodef", "", OpenFileGDB::FGFT_STRING, true, false, true, 20,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_dt_nodef", "", OpenFileGDB::FGFT_DATETIME, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_i64_nodef", "", OpenFileGDB::FGFT_INT64, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_time_nodef", "", OpenFileGDB::FGFT_TIME, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(
      u16_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f_dto_nodef", "", OpenFileGDB::FGFT_DATETIME_WITH_OFFSET, true,
          false, true, 0, OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(u16_table->Sync());
}

TEST_F(FileGDBTableWriteFieldsTest, RewriteTableOnNonEmptyTable) {
  const std::string table_path = std::string(kTestDir) + "/rewrite.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                            false, false));

  // Table starts with 0 nullable fields (and 1 non-nullable int32 so feature
  // payload size > 0)
  auto oid = std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table->CreateField(std::move(oid)));
  OGRField init_def;
  init_def.Integer = 1;
  auto init_int = std::make_unique<OpenFileGDB::FileGDBField>(
      "init_int", "", OpenFileGDB::FGFT_INT32, false, false, true, 0, init_def);
  ASSERT_TRUE(table->CreateField(std::move(init_int)));
  ASSERT_TRUE(table->Sync());

  // Insert 2 features
  auto flds = MakeUnsetFields(table->GetFieldCount());
  flds[1].Integer = 100;
  ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  ASSERT_TRUE(table->Sync());
  ASSERT_EQ(2, table->GetTotalRecordCount());

  // 1. Simulated error in RewriteTableToAddLastAddedField on a dedicated table
  {
    const std::string err_path = std::string(kTestDir) + "/simul_err.gdbtable";
    auto err_table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(err_table->Create(err_path.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                                  false, false));
    auto oid_err = std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    ASSERT_TRUE(err_table->CreateField(std::move(oid_err)));
    auto init_err = std::make_unique<OpenFileGDB::FileGDBField>(
        "init_int", "", OpenFileGDB::FGFT_INT32, false, false, true, 0,
        init_def);
    ASSERT_TRUE(err_table->CreateField(std::move(init_err)));
    ASSERT_TRUE(err_table->Sync());
    auto err_flds = MakeUnsetFields(err_table->GetFieldCount());
    err_flds[1].Integer = 100;
    ASSERT_TRUE(err_table->CreateFeature(err_flds, nullptr));

    CPLSetThreadLocalConfigOption(
        "OPENFILEGDB_SIMUL_ERROR_IN_RewriteTableToAddLastAddedField", "TRUE");
    absl::Cleanup reset_opt = [] {
      CPLSetThreadLocalConfigOption(
          "OPENFILEGDB_SIMUL_ERROR_IN_RewriteTableToAddLastAddedField",
          nullptr);
    };

    auto nullable_field = std::make_unique<OpenFileGDB::FileGDBField>(
        "simul_err", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD);
    EXPECT_FALSE(err_table->CreateField(std::move(nullable_field)));
  }

  // 2. Add 1st nullable field (nOldNullableFieldsSizeInBytes == 0 -> 1 byte)
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "null_1", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  // 3. Add 2nd..8th nullable fields ((m_nCountNullableFields % 8) != 0 -> no
  // rewrite)
  for (int i = 2; i <= 8; ++i) {
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        CPLSPrintf("null_%d", i), "", OpenFileGDB::FGFT_INT32, true, false,
        true, 0, OpenFileGDB::FileGDBField::UNSET_FIELD)));
  }

  // 4. Add 9th nullable field ((m_nCountNullableFields % 8) == 0 -> triggers
  // rewrite 1 -> 2 bytes)
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "null_9", "", OpenFileGDB::FGFT_INT16, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  // 5. Add non-nullable fields with defaults for ALL types in
  // RewriteTableToAddLastAddedField:
  char str_def_val[] = "constant_default";
  OGRField def_str;
  def_str.String = str_def_val;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_str", "", OpenFileGDB::FGFT_STRING, false, false, true, 0,
      def_str)));

  OGRField def_i16;
  def_i16.Integer = 42;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_i16", "", OpenFileGDB::FGFT_INT16, false, false, true, 0, def_i16)));

  OGRField def_i32;
  def_i32.Integer = 84;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_i32", "", OpenFileGDB::FGFT_INT32, false, false, true, 0, def_i32)));

  OGRField def_i64;
  def_i64.Integer64 = 10000000000LL;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_i64", "", OpenFileGDB::FGFT_INT64, false, false, true, 0, def_i64)));

  OGRField def_f32;
  def_f32.Real = 2.5f;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_f32", "", OpenFileGDB::FGFT_FLOAT32, false, false, true, 0,
      def_f32)));

  OGRField def_f64;
  def_f64.Real = 5.0;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_f64", "", OpenFileGDB::FGFT_FLOAT64, false, false, true, 0,
      def_f64)));

  OGRField def_dt;
  def_dt.Date.Year = 2024;
  def_dt.Date.Month = 8;
  def_dt.Date.Day = 10;
  def_dt.Date.Hour = 12;
  def_dt.Date.Minute = 0;
  def_dt.Date.Second = 0.0f;
  def_dt.Date.TZFlag = 100;
  auto req_dt = std::make_unique<OpenFileGDB::FileGDBField>(
      "req_dt", "", OpenFileGDB::FGFT_DATETIME, false, false, true, 0, def_dt);
  req_dt->SetHighPrecision();
  ASSERT_TRUE(table->CreateField(std::move(req_dt)));

  OGRField def_date;
  def_date.Date.Year = 2024;
  def_date.Date.Month = 9;
  def_date.Date.Day = 15;
  def_date.Date.Hour = 0;
  def_date.Date.Minute = 0;
  def_date.Date.Second = 0.0f;
  def_date.Date.TZFlag = 0;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_date", "", OpenFileGDB::FGFT_DATE, false, false, true, 0,
      def_date)));

  OGRField def_time;
  def_time.Date.Year = 0;
  def_time.Date.Month = 0;
  def_time.Date.Day = 0;
  def_time.Date.Hour = 18;
  def_time.Date.Minute = 45;
  def_time.Date.Second = 0.0f;
  def_time.Date.TZFlag = 0;
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_time", "", OpenFileGDB::FGFT_TIME, false, false, true, 0,
      def_time)));

  OGRField def_dto_tz;
  def_dto_tz.Date.Year = 2024;
  def_dto_tz.Date.Month = 10;
  def_dto_tz.Date.Day = 20;
  def_dto_tz.Date.Hour = 14;
  def_dto_tz.Date.Minute = 0;
  def_dto_tz.Date.Second = 0.0f;
  def_dto_tz.Date.TZFlag = 104;  // TZFlag > 1
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_dto_tz", "", OpenFileGDB::FGFT_DATETIME_WITH_OFFSET, false, false,
      true, 0, def_dto_tz)));

  OGRField def_dto_notz;
  def_dto_notz.Date.Year = 2024;
  def_dto_notz.Date.Month = 11;
  def_dto_notz.Date.Day = 25;
  def_dto_notz.Date.Hour = 8;
  def_dto_notz.Date.Minute = 0;
  def_dto_notz.Date.Second = 0.0f;
  def_dto_notz.Date.TZFlag = 0;  // TZFlag <= 1
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "req_dto_notz", "", OpenFileGDB::FGFT_DATETIME_WITH_OFFSET, false, false,
      true, 0, def_dto_notz)));

  ASSERT_TRUE(table->Sync());
  EXPECT_EQ(2, table->GetTotalRecordCount());
}

TEST_F(FileGDBTableWriteFieldsTest,
       WriteFieldDescriptorsRelocationAndShrinking) {
  const std::string table_path = std::string(kTestDir) + "/reloc.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                            false, false));

  // Add 2 fields with long names to have a reasonably sized descriptor
  std::string long_name(80, 'a');
  std::string long_alias(80, 'b');
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      long_name, long_alias, OpenFileGDB::FGFT_STRING, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->Sync());

  // Insert feature 1 so field descriptor is followed by feature data (not at
  // EOF)
  auto flds = MakeUnsetFields(table->GetFieldCount());
  ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  ASSERT_TRUE(table->Sync());

  // Now adding a new field causes abyBuffer.size() > m_nFieldDescLength,
  // triggering descriptor relocation to EOF and zeroing the old descriptor
  // section.
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "new_field_at_eof", "alias", OpenFileGDB::FGFT_INT32, true, false, true,
      0, OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->Sync());

  // Insert feature 2 so descriptor is again followed by feature data (not at
  // EOF)
  flds = MakeUnsetFields(table->GetFieldCount());
  ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  ASSERT_TRUE(table->Sync());

  // Alter long_name to a 2-char name, shrinking the descriptor size
  // (abyBuffer.size() <= m_nFieldDescLength and m_nFieldDescLength <
  // nOldFieldDescLength), which writes NUL bytes into the unused trailing
  // space.
  ASSERT_TRUE(table->AlterField(1, "aa", "bb", OpenFileGDB::FGFT_STRING, true,
                                0, OpenFileGDB::FileGDBField::UNSET_FIELD));
  ASSERT_TRUE(table->Sync());
}

TEST_F(FileGDBTableWriteFieldsTest, DeleteFieldOperations) {
  const std::string table_path = std::string(kTestDir) + "/delete.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_POINT,
                            false, false));

  // Field 0: nullable string
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "str0", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  // Field 1: ObjectID
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  // Field 2: Geometry
  std::vector<double> grid_res = {10.0};
  ASSERT_TRUE(
      table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
          "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid_res)));
  // Field 3: nullable int32
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "int3", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->Sync());

  // Create attribute indexes on field 0 ("str0") and field 1 ("ObjectID")
  ASSERT_TRUE(table->CreateIndex("idx_str0", "str0"));
  ASSERT_TRUE(table->CreateIndex("idx_oid", "ObjectID"));

  // Insert feature with non-empty values and geometry
  auto flds = MakeUnsetFields(table->GetFieldCount());
  std::string val = "some_text";
  flds[0].String = val.data();
  flds[3].Integer = 999;
  OGRPoint pt(10.0, 20.0);
  ASSERT_TRUE(table->CreateFeature(flds, &pt));
  ASSERT_TRUE(table->Sync());

  // 1. DeleteField error checks
  EXPECT_FALSE(table->DeleteField(-1));
  EXPECT_FALSE(table->DeleteField(999));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->DeleteField(table->GetGeomFieldIdx()));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Geometry field deletion not supported"));
  }

  // 2. Delete field 0 (str0, which is < m_iObjectIdField and < m_iGeomField)
  // This rewrites features (with geometry hack) and unlinks idx_str0.atx
  ASSERT_TRUE(table->DeleteField(0));
  ASSERT_TRUE(table->Sync());
  EXPECT_EQ(3, table->GetFieldCount());
  EXPECT_EQ(0, table->GetObjectIdFieldIdx());
  EXPECT_EQ(1, table->GetGeomFieldIdx());

  // 3. Delete ObjectID field (iField == m_iObjectIdField)
  ASSERT_TRUE(table->DeleteField(0));
  ASSERT_TRUE(table->Sync());
  EXPECT_EQ(2, table->GetFieldCount());
  EXPECT_EQ(-1, table->GetObjectIdFieldIdx());
  EXPECT_EQ(0, table->GetGeomFieldIdx());

  // 4. Read-only table cannot delete fields
  table->Close();
  auto ro = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(ro->Open(table_path.c_str(), /*bUpdate=*/false));
  EXPECT_FALSE(ro->DeleteField(0));
}

TEST_F(FileGDBTableWriteFieldsTest, AlterFieldAndAlterGeomField) {
  const std::string table_path = std::string(kTestDir) + "/alter.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_POINT,
                            false, false));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  std::vector<double> grid_res = {10.0};
  ASSERT_TRUE(
      table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
          "Shape", "GeomOrigAlias", true, "POINT", 0.0, 0.0, 1000.0, 0.001,
          grid_res)));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "col1", "alias1", OpenFileGDB::FGFT_STRING, true, false, true, 50,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "col2", "alias2", OpenFileGDB::FGFT_STRING, true, false, true, 50,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table->CreateIndex("idx_col1", "col1"));
  ASSERT_TRUE(table->CreateIndex("idx_col2", "LOWER(col2)"));
  ASSERT_TRUE(table->CreateIndex("idx_shape", "Shape"));
  ASSERT_TRUE(table->Sync());

  // 1. AlterField error cases
  EXPECT_FALSE(table->AlterField(-1, "bad", "", OpenFileGDB::FGFT_STRING, true,
                                 50, OpenFileGDB::FileGDBField::UNSET_FIELD));
  EXPECT_FALSE(table->AlterField(99, "bad", "", OpenFileGDB::FGFT_STRING, true,
                                 50, OpenFileGDB::FileGDBField::UNSET_FIELD));
  // Geom field rejected in AlterField
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->AlterField(1, "bad", "", OpenFileGDB::FGFT_GEOMETRY,
                                   true, 0,
                                   OpenFileGDB::FileGDBField::UNSET_FIELD));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        testing::HasSubstr("AlterField() not supported on geometry field"));
  }
  // Type change rejected
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->AlterField(2, "col1", "", OpenFileGDB::FGFT_INT32, true,
                                   0, OpenFileGDB::FileGDBField::UNSET_FIELD));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        testing::HasSubstr("does not support modifying the field type"));
  }
  // Nullability change rejected
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->AlterField(2, "col1", "", OpenFileGDB::FGFT_STRING,
                                   false, 50,
                                   OpenFileGDB::FileGDBField::UNSET_FIELD));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        testing::HasSubstr("does not support modifying the nullable state"));
  }
  // Renaming to existing field name rejected
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->AlterField(2, "col2", "", OpenFileGDB::FGFT_STRING,
                                   true, 50,
                                   OpenFileGDB::FileGDBField::UNSET_FIELD));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        testing::HasSubstr("cannot rename a field to an existing field name"));
  }

  // 2. AlterField success cases:
  // Rename field with normal index
  ASSERT_TRUE(table->AlterField(2, "col1_renamed", "new_alias1",
                                OpenFileGDB::FGFT_STRING, true, 60,
                                OpenFileGDB::FileGDBField::UNSET_FIELD));
  EXPECT_EQ("col1_renamed", table->GetField(2)->GetName());
  EXPECT_EQ("col1_renamed", table->GetField(2)->GetIndex()->GetExpression());

  // Rename field with LOWER() index
  ASSERT_TRUE(table->AlterField(3, "col2_renamed", "new_alias2",
                                OpenFileGDB::FGFT_STRING, true, 60,
                                OpenFileGDB::FileGDBField::UNSET_FIELD));
  EXPECT_EQ("col2_renamed", table->GetField(3)->GetName());
  EXPECT_EQ("LOWER(col2_renamed)",
            table->GetField(3)->GetIndex()->GetExpression());

  // Alter without rename (same name)
  ASSERT_TRUE(table->AlterField(2, "col1_renamed", "another_alias",
                                OpenFileGDB::FGFT_STRING, true, 70,
                                OpenFileGDB::FileGDBField::UNSET_FIELD));

  // 3. AlterGeomField error cases:
  // Nullability change rejected
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->AlterGeomField("Shape", "alias", false, "POINT"));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        testing::HasSubstr("does not support modifying the nullable state"));
  }

  // 4. AlterGeomField success case (renames geom field and updates spatial
  // index expression)
  ASSERT_TRUE(
      table->AlterGeomField("Shape_renamed", "GeomNewAlias", true, "POINT"));
  EXPECT_EQ("Shape_renamed", table->GetGeomField()->GetName());
  EXPECT_EQ(
      "Shape_renamed",
      table->GetField(table->GetGeomFieldIdx())->GetIndex()->GetExpression());

  // 5. Read-only table error checks
  table->Close();
  auto ro = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(ro->Open(table_path.c_str(), /*bUpdate=*/false));
  EXPECT_FALSE(ro->AlterField(1, "bad", "", OpenFileGDB::FGFT_STRING, true, 0,
                              OpenFileGDB::FileGDBField::UNSET_FIELD));
  EXPECT_FALSE(ro->AlterGeomField("bad", "", true, "POINT"));
}

}  // namespace
}  // namespace autotest2
