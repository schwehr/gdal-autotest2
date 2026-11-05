// Unit tests for OpenFileGDB index writing (filegdbindex_write.cpp).

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class WithQuietHandler {
 public:
  WithQuietHandler() { CPLPushErrorHandler(CPLQuietErrorHandler); }
  ~WithQuietHandler() { CPLPopErrorHandler(); }
};

std::vector<OGRField> MakeUnsetFields(int count) {
  std::vector<OGRField> flds(count);
  for (int i = 0; i < count; ++i) {
    flds[i] = OpenFileGDB::FileGDBField::UNSET_FIELD;
  }
  return flds;
}

class FileGDBIndexWriteTest : public ::testing::Test {
 protected:
  static constexpr const char* kTestDir = "/vsimem/filegdbindex_write_test";

  void SetUp() override {
    VSIRmdirRecursive(kTestDir);
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(FileGDBIndexWriteTest, CreateIndexValidationAndErrors) {
  const std::string table_path = std::string(kTestDir) + "/validation.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_POINT,
                            false, false));

  auto oid = std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table->CreateField(std::move(oid)));

  std::vector<double> grid_res = {10.0};
  auto geom = std::make_unique<OpenFileGDB::FileGDBGeomField>(
      "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid_res);
  ASSERT_TRUE(table->CreateField(std::move(geom)));

  auto int32_fld = std::make_unique<OpenFileGDB::FileGDBField>(
      "int32_fld", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table->CreateField(std::move(int32_fld)));

  auto bin_fld = std::make_unique<OpenFileGDB::FileGDBField>(
      "bin_fld", "", OpenFileGDB::FGFT_BINARY, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table->CreateField(std::move(bin_fld)));

  ASSERT_TRUE(table->Sync());

  // 1. Read-only table returns false on CreateIndex, RemoveIndices,
  // RefreshIndices
  {
    auto table_ro = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table_ro->Open(table_path.c_str(), false));
    EXPECT_FALSE(table_ro->CreateIndex("idx_ro", "int32_fld"));
    // Calling RemoveIndices / RefreshIndices on read-only returns early
    // (!m_bUpdate) We cannot call private methods directly, but Sync on
    // read-only table is a no-op
    EXPECT_TRUE(table_ro->Sync());
  }

  // 2. Name validation errors
  {
    WithQuietHandler quiet;

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("", "int32_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("must start with a letter"));

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("1idx", "int32_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("must start with a letter"));

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("idx-invalid", "int32_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("alpha numeric character or _"));

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("a12345678901234567", "int32_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("greater than 16 characters"));

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("idx_none", "no_such_field"));
    EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Cannot find field"));

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("idx_bin", "bin_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Unsupported field type"));
  }

  // 3. Valid index creation
  ASSERT_TRUE(table->CreateIndex("idx_int", "int32_fld"));
  ASSERT_TRUE(table->CreateIndex("idx_oid", "ObjectID"));
  ASSERT_TRUE(table->CreateIndex("idx_geom", "Shape"));

  // 4. Duplicate name and already registered index
  {
    WithQuietHandler quiet;

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("IDX_INT", "int32_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("same name already exists"));

    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("idx_int2", "int32_fld"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("already a registered index"));
  }

  // 5. CreateGdbIndexesFile: field renamed via AlterField before Sync()
  {
    int iIntField = table->GetFieldIdx("int32_fld");
    ASSERT_GE(iIntField, 0);
    ASSERT_TRUE(table->AlterField(iIntField, "renamed_int", "",
                                  OpenFileGDB::FGFT_INT32, true, 0,
                                  OpenFileGDB::FileGDBField::UNSET_FIELD));
    EXPECT_TRUE(table->Sync());
  }
}

TEST_F(FileGDBIndexWriteTest, AllAttributeIndexTypesAndRefresh) {
  const std::string table_path = std::string(kTestDir) + "/attr_types.gdbtable";
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(table->Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                            false, false));

  // Field 0: ObjectID
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  // Field 1..11: attribute types
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_i16", "", OpenFileGDB::FGFT_INT16, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_i32", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_i64", "", OpenFileGDB::FGFT_INT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_f32", "", OpenFileGDB::FGFT_FLOAT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_f64", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_dt", "", OpenFileGDB::FGFT_DATETIME, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_d", "", OpenFileGDB::FGFT_DATE, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_t", "", OpenFileGDB::FGFT_TIME, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_dto", "", OpenFileGDB::FGFT_DATETIME_WITH_OFFSET, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_str", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "f_str_low", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table->Sync());

  // Insert feature 1: fully populated with values (including string >
  // MAX_CAR_COUNT_INDEXED_STR)
  {
    auto flds = MakeUnsetFields(table->GetFieldCount());
    flds[1].Integer = 100;
    flds[2].Integer = 200;
    flds[3].Integer64 = 3000000000LL;
    flds[4].Real = 1.25f;
    flds[5].Real = 2.75;
    flds[6].Date = {2024, 2, 10, 14, 30, 0, 0};
    flds[7].Date = {2024, 2, 10, 0, 0, 0, 0};
    flds[8].Date = {0, 0, 0, 14, 30, 0, 0};
    flds[9].Date = {2024, 2, 10, 14, 30, 0, 100};
    char str1[] = "Short";
    flds[10].String = str1;
    char str_long[] = "MixedCaseAlpha1234567890EXTRA_LONG_STRING_OVER_LIMIT";
    flds[11].String = str_long;
    ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  }

  // Insert feature 2: all fields null / unset
  {
    auto flds = MakeUnsetFields(table->GetFieldCount());
    ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  }

  // Insert feature 3: different values
  {
    auto flds = MakeUnsetFields(table->GetFieldCount());
    flds[1].Integer = 50;
    flds[2].Integer = 150;
    flds[3].Integer64 = 100000LL;
    flds[4].Real = 0.5f;
    flds[5].Real = 1.5;
    flds[6].Date = {2023, 1, 1, 8, 0, 0, 0};
    flds[7].Date = {2023, 1, 1, 0, 0, 0, 0};
    flds[8].Date = {0, 0, 0, 8, 0, 0, 0};
    flds[9].Date = {2023, 1, 1, 8, 0, 0, 100};
    char str2[] = "Another";
    flds[10].String = str2;
    char str_low[] = "ALLCAPSSTRING";
    flds[11].String = str_low;
    ASSERT_TRUE(table->CreateFeature(flds, nullptr));
  }
  ASSERT_TRUE(table->Sync());

  // Create attribute indexes on all fields
  ASSERT_TRUE(table->CreateIndex("idx_i16", "f_i16"));
  ASSERT_TRUE(table->CreateIndex("idx_i32", "f_i32"));
  ASSERT_TRUE(table->CreateIndex("idx_i64", "f_i64"));
  ASSERT_TRUE(table->CreateIndex("idx_f32", "f_f32"));
  ASSERT_TRUE(table->CreateIndex("idx_f64", "f_f64"));
  ASSERT_TRUE(table->CreateIndex("idx_dt", "f_dt"));
  ASSERT_TRUE(table->CreateIndex("idx_d", "f_d"));
  ASSERT_TRUE(table->CreateIndex("idx_t", "f_t"));
  ASSERT_TRUE(table->CreateIndex("idx_dto", "f_dto"));
  ASSERT_TRUE(table->CreateIndex("idx_str", "f_str"));
  ASSERT_TRUE(table->CreateIndex("idx_str_low", "LOWER(f_str_low)"));
  ASSERT_TRUE(table->Sync());

  // Verify created files exist
  const std::string gdbindexes =
      std::string(kTestDir) + "/attr_types.gdbindexes";
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(gdbindexes.c_str(), &sStat));
  EXPECT_EQ(
      0, VSIStatL((std::string(kTestDir) + "/attr_types.idx_i16.atx").c_str(),
                  &sStat));
  EXPECT_EQ(
      0,
      VSIStatL((std::string(kTestDir) + "/attr_types.idx_str_low.atx").c_str(),
               &sStat));

  // Insert feature 4 to trigger RefreshIndices() on Sync()
  {
    auto flds = MakeUnsetFields(table->GetFieldCount());
    flds[1].Integer = 10;
    flds[2].Integer = 20;
    flds[3].Integer64 = 30LL;
    flds[4].Real = 0.1f;
    flds[5].Real = 0.2;
    flds[6].Date = {2022, 5, 5, 5, 5, 0, 0};
    flds[7].Date = {2022, 5, 5, 0, 0, 0, 0};
    flds[8].Date = {0, 0, 0, 5, 5, 0, 0};
    flds[9].Date = {2022, 5, 5, 5, 5, 0, 0};
    char str_new[] = "New";
    flds[10].String = str_new;
    char str_low_new[] = "NewLow";
    flds[11].String = str_low_new;
    ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    ASSERT_TRUE(table->Sync());
  }

  // Test invalid UTF-8 string triggering CPLRecodeToWChar failure
  {
    const std::string bad_utf8_path =
        std::string(kTestDir) + "/bad_utf8.gdbtable";
    auto bad_table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(bad_table->Create(bad_utf8_path.c_str(), 4,
                                  OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(
        bad_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
            "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
            OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(
        bad_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
            "str_field", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
            OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(bad_table->Sync());

    auto flds = MakeUnsetFields(bad_table->GetFieldCount());
    char invalid_utf8[] = "\xFF\xFF\xFF";
    flds[1].String = invalid_utf8;
    ASSERT_TRUE(bad_table->CreateFeature(flds, nullptr));
    ASSERT_TRUE(bad_table->Sync());

    WithQuietHandler quiet;
    EXPECT_TRUE(bad_table->CreateIndex("idx_bad", "str_field"));
  }
}

TEST_F(FileGDBIndexWriteTest, WriteIndexDepths1To4AndOverflow) {
  CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE", "2");
  absl::Cleanup reset_opt = [] {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE",
                                  nullptr);
  };

  // 1. Depth 1 (2 features <= 2 per page)
  {
    const std::string path = std::string(kTestDir) + "/depth1.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());

    for (int i = 0; i < 2; ++i) {
      auto flds = MakeUnsetFields(table->GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(table->Sync());
    ASSERT_TRUE(table->CreateIndex("idx_d1", "val"));
  }

  // 2. Depth 2 (5 features: ceil(5/2) = 3 feature pages, fits in depth 2 root
  // page)
  {
    const std::string path = std::string(kTestDir) + "/depth2.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());

    for (int i = 0; i < 5; ++i) {
      auto flds = MakeUnsetFields(table->GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(table->Sync());
    ASSERT_TRUE(table->CreateIndex("idx_d2", "val"));
  }

  // 3. Depth 3: 9 features (nNumPagesLevel2 == 2) and 11 features
  // (nNumPagesLevel2 == 3)
  {
    const std::string path = std::string(kTestDir) + "/depth3_9.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());

    for (int i = 0; i < 9; ++i) {
      auto flds = MakeUnsetFields(table->GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(table->Sync());
    ASSERT_TRUE(table->CreateIndex("idx_d3_9", "val"));
  }

  {
    const std::string path = std::string(kTestDir) + "/depth3_11.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());

    for (int i = 0; i < 11; ++i) {
      auto flds = MakeUnsetFields(table->GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(table->Sync());
    ASSERT_TRUE(table->CreateIndex("idx_d3_11", "val"));
  }

  // 4. Depth 4 (20 features: reaches depth 4 B-tree)
  {
    const std::string path = std::string(kTestDir) + "/depth4.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());

    for (int i = 0; i < 20; ++i) {
      auto flds = MakeUnsetFields(table->GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(table->Sync());
    ASSERT_TRUE(table->CreateIndex("idx_d4", "val"));
  }

  // 5. Overflow: 31 features > max 30 for depth 4 with max_per_page=2
  {
    const std::string path = std::string(kTestDir) + "/overflow.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());

    for (int i = 0; i < 31; ++i) {
      auto flds = MakeUnsetFields(table->GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(table->Sync());

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateIndex("idx_overflow", "val"));
    EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Write error during"));

    // Also test spatial index overflow check for .spx write error
    const std::string spx_path =
        std::string(kTestDir) + "/overflow_spx.gdbtable";
    auto spx_table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(spx_table->Create(spx_path.c_str(), 4, OpenFileGDB::FGTGT_POINT,
                                  false, false));
    ASSERT_TRUE(
        spx_table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
            "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
            OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> spx_grid = {1.0};
    ASSERT_TRUE(
        spx_table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, spx_grid)));
    ASSERT_TRUE(spx_table->Sync());
    for (int i = 0; i < 31; ++i) {
      auto flds = MakeUnsetFields(spx_table->GetFieldCount());
      OGRPoint pt(i * 1.0, i * 1.0);
      ASSERT_TRUE(spx_table->CreateFeature(flds, &pt));
    }
    ASSERT_TRUE(spx_table->Sync());
    CPLErrorReset();
    EXPECT_FALSE(spx_table->CreateSpatialIndex());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Write error during .spx generation"));
  }

  // 6. Test clamping of OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE to 2 and to max
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE",
                                  "1");  // Clamps to 2
    const std::string path = std::string(kTestDir) + "/clamp_low.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());
    auto flds = MakeUnsetFields(table->GetFieldCount());
    flds[1].Integer = 1;
    ASSERT_TRUE(table->CreateFeature(flds, nullptr));
    ASSERT_TRUE(table->Sync());
    EXPECT_TRUE(table->CreateIndex("idx_clamp", "val"));

    CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE",
                                  "99999");  // Clamps to max
    EXPECT_TRUE(table->Sync());
  }

  // 7. Test OPENFILEGDB_FORCE_SPX_DEPTH=2 with 1 feature (hits
  // nNumDirectChildren == 1)
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_FORCE_SPX_DEPTH", "2");
    absl::Cleanup reset_force = [] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_FORCE_SPX_DEPTH", nullptr);
    };

    const std::string path = std::string(kTestDir) + "/force_depth.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRPoint pt(10.0, 20.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt));
    ASSERT_TRUE(table->Sync());

    EXPECT_TRUE(table->CreateSpatialIndex());
  }
}

TEST_F(FileGDBIndexWriteTest, ComputeOptimalGridResolutionPointAndMultiPoint) {
  // 1. Table with no geometry field or empty table
  {
    const std::string path = std::string(kTestDir) + "/no_geom.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table->Sync());
    table->ComputeOptimalSpatialIndexGridResolution();
    EXPECT_FALSE(table->CreateSpatialIndex());
  }

  // 2. Geometry field with size() > 3 or empty grid resolution
  {
    const std::string path = std::string(kTestDir) + "/invalid_grid.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> four_grids = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, four_grids)));
    ASSERT_TRUE(table->Sync());
    EXPECT_FALSE(table->CreateSpatialIndex());
  }

  // 3. FGTGT_MULTIPATCH
  {
    const std::string path = std::string(kTestDir) + "/multipatch.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_MULTIPATCH,
                              false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {10.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "MULTIPATCH", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CreateSpatialIndex());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                testing::HasSubstr("Multipatch not supported"));
  }

  // 4. FGTGT_POINT branches:
  // a) dfArea != 0
  {
    const std::string path = std::string(kTestDir) + "/pt_area.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRPoint pt1(0.0, 0.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt1));
    OGRPoint pt2(10.0, 10.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt2));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_NEAR(std::sqrt(100.0 / 2), res[0], 0.01);
    EXPECT_TRUE(table->CreateSpatialIndex());
  }

  // b) dfArea == 0, XMax > XMin
  {
    const std::string path = std::string(kTestDir) + "/pt_dx.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRPoint pt1(0.0, 5.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt1));
    OGRPoint pt2(10.0, 5.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt2));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_NEAR(5.0, res[0], 0.01);
  }

  // c) dfArea == 0, YMax > YMin
  {
    const std::string path = std::string(kTestDir) + "/pt_dy.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRPoint pt1(5.0, 0.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt1));
    OGRPoint pt2(5.0, 10.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt2));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_NEAR(5.0, res[0], 0.01);
  }

  // d) Single point (XMax == XMin && YMax == YMin) with initial grid {0.0}
  {
    const std::string path = std::string(kTestDir) + "/pt_single.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {0.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRPoint pt(5.0, 5.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    EXPECT_FALSE(table->CreateSpatialIndex());
  }

  // e) With OPENFILEGDB_GRID_SIZE config option
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_GRID_SIZE", "2.5");
    absl::Cleanup reset_grid = [] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_GRID_SIZE", nullptr);
    };

    const std::string path = std::string(kTestDir) + "/pt_grid_opt.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "POINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRPoint pt(5.0, 5.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt));
    ASSERT_TRUE(table->Sync());

    EXPECT_TRUE(table->CreateSpatialIndex());
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_DOUBLE_EQ(2.5, res[0]);
  }

  // 5. FGTGT_MULTIPOINT branches:
  // a) dfArea != 0
  {
    const std::string path = std::string(kTestDir) + "/mpt_area.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_MULTIPOINT,
                              false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "MULTIPOINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRMultiPoint mpt;
    OGRPoint pt1(0.0, 0.0);
    OGRPoint pt2(10.0, 10.0);
    mpt.addGeometry(&pt1);
    mpt.addGeometry(&pt2);
    ASSERT_TRUE(table->CreateFeature(flds, &mpt));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_NEAR(std::sqrt(100.0 / 2), res[0], 0.01);
    EXPECT_TRUE(table->CreateSpatialIndex());
  }

  // b) dfArea == 0, XMax > XMin
  {
    const std::string path = std::string(kTestDir) + "/mpt_dx.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_MULTIPOINT,
                              false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "MULTIPOINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRMultiPoint mpt;
    OGRPoint pt1(0.0, 5.0);
    OGRPoint pt2(10.0, 5.0);
    mpt.addGeometry(&pt1);
    mpt.addGeometry(&pt2);
    ASSERT_TRUE(table->CreateFeature(flds, &mpt));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_NEAR(5.0, res[0], 0.01);
  }

  // c) dfArea == 0, YMax > YMin
  {
    const std::string path = std::string(kTestDir) + "/mpt_dy.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_MULTIPOINT,
                              false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "MULTIPOINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRMultiPoint mpt;
    OGRPoint pt1(5.0, 0.0);
    OGRPoint pt2(5.0, 10.0);
    mpt.addGeometry(&pt1);
    mpt.addGeometry(&pt2);
    ASSERT_TRUE(table->CreateFeature(flds, &mpt));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    const auto& res = table->GetSpatialIndexGridResolution();
    ASSERT_EQ(1u, res.size());
    EXPECT_NEAR(5.0, res[0], 0.01);
  }

  // d) dfArea == 0, single point multipoint with initial grid {0.0}
  {
    const std::string path = std::string(kTestDir) + "/mpt_single.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_MULTIPOINT,
                              false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {0.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, "MULTIPOINT", 0.0, 0.0, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    OGRMultiPoint mpt;
    OGRPoint pt(5.0, 5.0);
    mpt.addGeometry(&pt);
    ASSERT_TRUE(table->CreateFeature(flds, &mpt));
    ASSERT_TRUE(table->Sync());

    table->ComputeOptimalSpatialIndexGridResolution();
    EXPECT_FALSE(table->CreateSpatialIndex());
  }
}

TEST_F(FileGDBIndexWriteTest, CreateSpatialIndexLinesAndPolygons) {
  CPLSetThreadLocalConfigOption("OPENFILEGDB_GRID_SIZE", "1.0");
  absl::Cleanup reset_grid = [] {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_GRID_SIZE", nullptr);
  };

  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  srs.SetUTM(31, TRUE);
  char* pszWKT = nullptr;
  srs.exportToWkt(&pszWKT);
  std::string utm_wkt = pszWKT;
  CPLFree(pszWKT);

  // 1. Lines table
  {
    const std::string path = std::string(kTestDir) + "/lines.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_LINE, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, utm_wkt, -1e8, -1e8, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    // Register index on ObjectID and Shape to test lines 50, 57, 88, 95 in
    // RefreshIndices/RemoveIndices
    ASSERT_TRUE(table->CreateIndex("idx_oid", "ObjectID"));
    ASSERT_TRUE(table->CreateIndex("idx_spx", "Shape"));

    auto flds = MakeUnsetFields(table->GetFieldCount());

    // a) Empty line string (< 2 points) - hits line 883
    OGRLineString ls_empty;
    ASSERT_TRUE(table->CreateFeature(flds, &ls_empty));

    // b) Vertical lines (both directions: dfYEnd > dfY and dfYEnd < dfY)
    OGRLineString ls_v1;
    ls_v1.addPoint(1.0, 1.0);
    ls_v1.addPoint(1.0, 5.0);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_v1));

    OGRLineString ls_v2;
    ls_v2.addPoint(2.0, 5.0);
    ls_v2.addPoint(2.0, 1.0);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_v2));

    // c) Horizontal lines (both directions: dfXEnd > dfX and dfXEnd < dfX
    // hitting line 943)
    OGRLineString ls_h1;
    ls_h1.addPoint(1.0, 1.0);
    ls_h1.addPoint(5.0, 1.0);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_h1));

    OGRLineString ls_h2;
    ls_h2.addPoint(5.0, 2.0);
    ls_h2.addPoint(1.0, 2.0);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_h2));

    // d) Sloped lines: positive slope, negative slope, reversed X, shallow
    // slope
    OGRLineString ls_s1;
    ls_s1.addPoint(0.1, 0.1);
    ls_s1.addPoint(10.1, 8.1);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_s1));

    OGRLineString ls_s2;
    ls_s2.addPoint(0.1, 8.1);
    ls_s2.addPoint(10.1, 0.1);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_s2));

    OGRLineString ls_s3;
    ls_s3.addPoint(10.1, 0.1);
    ls_s3.addPoint(0.1, 8.1);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_s3));

    OGRLineString ls_s4;
    ls_s4.addPoint(0.1, 1.1);
    ls_s4.addPoint(
        10.1, 1.4);  // Shallow slope: step to right without changing scanline
    ASSERT_TRUE(table->CreateFeature(flds, &ls_s4));

    // e) Line with coordinates out of 31-bit range - hits line 901
    OGRLineString ls_huge;
    ls_huge.addPoint(0.0, 0.0);
    ls_huge.addPoint(2e9, 2e9);
    ASSERT_TRUE(table->CreateFeature(flds, &ls_huge));

    // f) MultiLineString
    OGRMultiLineString mls;
    mls.addGeometry(&ls_v1);
    mls.addGeometry(&ls_h1);
    ASSERT_TRUE(table->CreateFeature(flds, &mls));

    // g) CircularString, CompoundCurve, MultiCurve
    OGRCircularString cs;
    cs.addPoint(0.0, 0.0);
    cs.addPoint(1.0, 1.0);
    cs.addPoint(2.0, 0.0);
    ASSERT_TRUE(table->CreateFeature(flds, &cs));

    OGRCompoundCurve cc;
    OGRLineString ls_ext;
    ls_ext.addPoint(2.0, 0.0);
    ls_ext.addPoint(3.0, 0.0);
    cc.addCurveDirectly(cs.clone());
    cc.addCurveDirectly(ls_ext.clone());
    ASSERT_TRUE(table->CreateFeature(flds, &cc));

    OGRMultiCurve mc;
    mc.addGeometryDirectly(cs.clone());
    ASSERT_TRUE(table->CreateFeature(flds, &mc));

    ASSERT_TRUE(table->Sync());
    EXPECT_TRUE(table->CreateSpatialIndex());

    // Trigger RefreshIndices() to test lines 50, 57, 88, 95
    ASSERT_TRUE(table->CreateFeature(flds, &ls_v1));
    ASSERT_TRUE(table->Sync());

    // Also test ComputeOptimalSpatialIndexGridResolution on lines
    table->ComputeOptimalSpatialIndexGridResolution();
  }

  // 2. Polygons table
  {
    const std::string path = std::string(kTestDir) + "/polys.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POLYGON,
                              false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, utm_wkt, -1e8, -1e8, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());

    // a) Empty polygon - hits line 1039
    OGRPolygon poly_empty;
    ASSERT_TRUE(table->CreateFeature(flds, &poly_empty));

    // b) Polygon with exterior ring and interior hole, plus horizontal edge at
    // Y = 2.5 This hits dy1 == dy2 (lines 1092-1116)
    OGRPolygon poly;
    OGRLinearRing ext;
    ext.addPoint(0.0, 0.0);
    ext.addPoint(10.0, 0.0);
    ext.addPoint(10.0, 2.5);
    ext.addPoint(0.0, 2.5);
    ext.addPoint(0.0, 0.0);
    poly.addRing(&ext);
    OGRLinearRing hole;
    hole.addPoint(2.0, 0.5);
    hole.addPoint(8.0, 0.5);
    hole.addPoint(8.0, 1.5);
    hole.addPoint(2.0, 1.5);
    hole.addPoint(2.0, 0.5);
    poly.addRing(&hole);
    ASSERT_TRUE(table->CreateFeature(flds, &poly));

    // c) MultiPolygon, CurvePolygon, MultiSurface
    OGRMultiPolygon mpoly;
    mpoly.addGeometry(&poly);
    ASSERT_TRUE(table->CreateFeature(flds, &mpoly));

    OGRCurvePolygon cpoly;
    OGRCircularString cs;
    cs.addPoint(-2.0, 0.0);
    cs.addPoint(0.0, 2.0);
    cs.addPoint(2.0, 0.0);
    cs.addPoint(0.0, -2.0);
    cs.addPoint(-2.0, 0.0);
    cpoly.addRingDirectly(cs.clone());
    ASSERT_TRUE(table->CreateFeature(flds, &cpoly));

    OGRMultiSurface ms;
    ms.addGeometry(&cpoly);
    ASSERT_TRUE(table->CreateFeature(flds, &ms));

    ASSERT_TRUE(table->Sync());
    EXPECT_TRUE(table->CreateSpatialIndex());

    // Test RemoveIndices via Sync after updating a feature
    flds[0].Integer = 1;
    ASSERT_TRUE(table->UpdateFeature(1, flds, &poly));
    ASSERT_TRUE(table->Sync());
  }

  // 3. Dedicated table for out-of-range point and polygon Y clamping
  {
    const std::string path = std::string(kTestDir) + "/clamp_test.gdbtable";
    auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
    ASSERT_TRUE(
        table->Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
    ASSERT_TRUE(table->CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD)));
    std::vector<double> grid = {1.0};
    ASSERT_TRUE(
        table->CreateField(std::make_unique<OpenFileGDB::FileGDBGeomField>(
            "Shape", "", true, utm_wkt, -1e8, -1e8, 1000.0, 0.001, grid)));
    ASSERT_TRUE(table->Sync());

    auto flds = MakeUnsetFields(table->GetFieldCount());
    // Point with X > 2^30 - hits line 873
    OGRPoint pt_huge(2e9, 0.0);
    ASSERT_TRUE(table->CreateFeature(flds, &pt_huge));
    ASSERT_TRUE(table->Sync());
    EXPECT_TRUE(table->CreateSpatialIndex());
  }
}

}  // namespace
