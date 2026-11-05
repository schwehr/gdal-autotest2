// Unit tests for OpenFileGDB index reading and iterators (filegdbindex.cpp).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using OpenFileGDB::FileGDBField;
using OpenFileGDB::FileGDBFieldType;
using OpenFileGDB::FileGDBGeomField;
using OpenFileGDB::FileGDBIndex;
using OpenFileGDB::FileGDBIterator;
using OpenFileGDB::FileGDBSpatialIndexIterator;
using OpenFileGDB::FileGDBSQLOp;
using OpenFileGDB::FileGDBTable;
using OpenFileGDB::FileGDBTableGeometryType;

using OpenFileGDB::FGFT_BINARY;
using OpenFileGDB::FGFT_DATE;
using OpenFileGDB::FGFT_DATETIME;
using OpenFileGDB::FGFT_DATETIME_WITH_OFFSET;
using OpenFileGDB::FGFT_FLOAT32;
using OpenFileGDB::FGFT_FLOAT64;
using OpenFileGDB::FGFT_GLOBALID;
using OpenFileGDB::FGFT_GUID;
using OpenFileGDB::FGFT_INT16;
using OpenFileGDB::FGFT_INT32;
using OpenFileGDB::FGFT_INT64;
using OpenFileGDB::FGFT_OBJECTID;
using OpenFileGDB::FGFT_STRING;
using OpenFileGDB::FGFT_TIME;
using OpenFileGDB::FGFT_XML;

using OpenFileGDB::FGSO_EQ;
using OpenFileGDB::FGSO_GE;
using OpenFileGDB::FGSO_GT;
using OpenFileGDB::FGSO_ILIKE;
using OpenFileGDB::FGSO_ISNOTNULL;
using OpenFileGDB::FGSO_LE;
using OpenFileGDB::FGSO_LT;

using OpenFileGDB::FGTGT_LINE;
using OpenFileGDB::FGTGT_NONE;
using OpenFileGDB::FGTGT_POINT;
using OpenFileGDB::FGTGT_POLYGON;

class WithQuietHandler {
 public:
  WithQuietHandler() { CPLPushErrorHandler(CPLQuietErrorHandler); }
  ~WithQuietHandler() { CPLPopErrorHandler(); }
};

std::vector<OGRField> MakeUnsetFields(int count) {
  std::vector<OGRField> flds(count);
  for (int i = 0; i < count; ++i) {
    flds[i] = FileGDBField::UNSET_FIELD;
  }
  return flds;
}

class FileGDBIndexTest : public ::testing::Test {
 protected:
  static constexpr const char* kTestDir = "/vsimem/filegdbindex_test";

  void SetUp() override {
    VSIRmdirRecursive(kTestDir);
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

// -----------------------------------------------------------------------------
// Suite 1: FileGDBIndex static/member methods, validation & error cases
// -----------------------------------------------------------------------------
TEST_F(FileGDBIndexTest, FileGDBIndexMethodsAndTrailerErrors) {
  // 1. GetFieldNameFromExpression & GetFieldName
  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("my_field"), "my_field");
  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("LOWER(my_field)"),
            "my_field");
  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("lower(MY_FIELD)"),
            "MY_FIELD");
  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("LOWER("), "LOWER(");

  // 2. GetMaxWidthInBytes error paths
  {
    const std::string path = std::string(kTestDir) + "/width_test.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(path.c_str(), 4, FGTGT_NONE, false, false));
    ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table.CreateField(
        std::make_unique<FileGDBField>("str_val", "", FGFT_STRING, true, false,
                                       true, 0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table.Sync());

    ASSERT_TRUE(table.CreateIndex("str_val_idx", "str_val"));
    ASSERT_TRUE(table.Sync());
    const FileGDBIndex* idx = table.GetField(1)->GetIndex();
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(idx->GetIndexName(), "str_val_idx");
    EXPECT_EQ(idx->GetExpression(), "str_val");
    EXPECT_EQ(idx->GetFieldName(), "str_val");

    const std::string atx_path =
        std::string(kTestDir) + "/width_test.str_val_idx.atx";
    VSIUnlink(atx_path.c_str());

    // Non-existent .atx file -> returns 0
    EXPECT_EQ(idx->GetMaxWidthInBytes(&table), 0);

    // File too small (< 4096 + 22) -> returns 0
    VSILFILE* fp = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    GByte small_buf[100] = {0};
    VSIFWriteL(small_buf, 1, sizeof(small_buf), fp);
    VSIFCloseL(fp);
    EXPECT_EQ(idx->GetMaxWidthInBytes(&table), 0);

    // File with bad version (e.g. 99)
    std::vector<GByte> buf_bad_ver(4096 + 22, 0);
    uint32_t bad_ver = 99;
    memcpy(buf_bad_ver.data() + buf_bad_ver.size() - sizeof(uint32_t), &bad_ver,
           4);
    fp = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(buf_bad_ver.data(), 1, buf_bad_ver.size(), fp);
    VSIFCloseL(fp);
    EXPECT_EQ(idx->GetMaxWidthInBytes(&table), 0);

    // Version 2 but file too small (< 65536 + 30)
    std::vector<GByte> buf_v2_small(4096 + 30, 0);
    uint32_t v2 = 2;
    memcpy(buf_v2_small.data() + buf_v2_small.size() - sizeof(uint32_t), &v2,
           4);
    fp = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(buf_v2_small.data(), 1, buf_v2_small.size(), fp);
    VSIFCloseL(fp);
    EXPECT_EQ(idx->GetMaxWidthInBytes(&table), 0);

    // Valid V1 trailer returning width
    std::vector<GByte> buf_v1(4096 + 22, 0);
    buf_v1[buf_v1.size() - 22] = 40;  // maxWidth
    uint32_t v1 = 1;
    memcpy(buf_v1.data() + buf_v1.size() - sizeof(uint32_t), &v1, 4);
    fp = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(buf_v1.data(), 1, buf_v1.size(), fp);
    VSIFCloseL(fp);
    EXPECT_EQ(idx->GetMaxWidthInBytes(&table), 40);

    // Valid V2 trailer returning width
    std::vector<GByte> buf_v2(65536 + 30, 0);
    buf_v2[buf_v2.size() - 30] = 60;  // maxWidth
    memcpy(buf_v2.data() + buf_v2.size() - sizeof(uint32_t), &v2, 4);
    fp = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(buf_v2.data(), 1, buf_v2.size(), fp);
    VSIFCloseL(fp);
    EXPECT_EQ(idx->GetMaxWidthInBytes(&table), 60);

    VSIUnlink(atx_path.c_str());
  }

  // 3. SetConstraint and ReadTrailer validation and error cases
  {
    const std::string path = std::string(kTestDir) + "/errors.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(path.c_str(), 4, FGTGT_NONE, false, false));
    ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table.CreateField(
        std::make_unique<FileGDBField>("val_int", "", FGFT_INT32, true, false,
                                       true, 0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table.CreateField(
        std::make_unique<FileGDBField>("str_val", "", FGFT_STRING, true, false,
                                       true, 0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table.CreateField(
        std::make_unique<FileGDBField>("str_low", "", FGFT_STRING, true, false,
                                       true, 0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(table.Sync());

    std::vector<OGRField> flds = MakeUnsetFields(table.GetFieldCount());
    flds[1].Integer = 10;
    char s_val[] = "test";
    flds[2].String = s_val;
    flds[3].String = s_val;
    ASSERT_TRUE(table.CreateFeature(flds, nullptr));
    ASSERT_TRUE(table.Sync());

    ASSERT_TRUE(table.CreateIndex("idx_int", "val_int"));
    ASSERT_TRUE(table.CreateIndex("idx_str", "str_val"));
    ASSERT_TRUE(table.CreateIndex("idx_low", "LOWER(str_low)"));

    // Invalid field index (< 0 or >= GetFieldCount)
    {
      WithQuietHandler quiet;
      EXPECT_EQ(FileGDBIterator::Build(&table, -1, TRUE, FGSO_ISNOTNULL,
                                       OFTMaxType, nullptr),
                nullptr);
      EXPECT_EQ(FileGDBIterator::Build(&table, 999, TRUE, FGSO_ISNOTNULL,
                                       OFTMaxType, nullptr),
                nullptr);
    }

    // Field without index (OBJECTID at index 0)
    {
      WithQuietHandler quiet;
      EXPECT_EQ(FileGDBIterator::Build(&table, 0, TRUE, FGSO_ISNOTNULL,
                                       OFTMaxType, nullptr),
                nullptr);
    }

    // ILIKE on normal string index (fails)
    {
      OGRField s;
      s.String = s_val;
      WithQuietHandler quiet;
      EXPECT_EQ(
          FileGDBIterator::Build(&table, 2, TRUE, FGSO_ILIKE, OFTString, &s),
          nullptr);
    }

    // Non-ILIKE / non-ISNOTNULL on LOWER(...) index (fails)
    {
      OGRField s;
      s.String = s_val;
      WithQuietHandler quiet;
      EXPECT_EQ(FileGDBIterator::Build(&table, 3, TRUE, FGSO_EQ, OFTString, &s),
                nullptr);
    }

    // ILIKE with %, _, or non-ASCII on LOWER(...) index (fails)
    {
      WithQuietHandler quiet;
      OGRField s;
      char pat_pct[] = "te%st";
      s.String = pat_pct;
      EXPECT_EQ(
          FileGDBIterator::Build(&table, 3, TRUE, FGSO_ILIKE, OFTString, &s),
          nullptr);

      char pat_us[] = "te_st";
      s.String = pat_us;
      EXPECT_EQ(
          FileGDBIterator::Build(&table, 3, TRUE, FGSO_ILIKE, OFTString, &s),
          nullptr);

      char non_ascii[] = "t\xC3\xA9st";
      s.String = non_ascii;
      EXPECT_EQ(
          FileGDBIterator::Build(&table, 3, TRUE, FGSO_ILIKE, OFTString, &s),
          nullptr);
    }

    // Mismatched OGRFieldType
    {
      WithQuietHandler quiet;
      OGRField wrong_fld;
      wrong_fld.Real = 3.14;
      EXPECT_EQ(
          FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTReal, &wrong_fld),
          nullptr);
    }

    // String search value exceeding index string width
    {
      WithQuietHandler quiet;
      std::string too_long(500, 'Z');
      OGRField long_fld;
      long_fld.String = const_cast<char*>(too_long.c_str());
      EXPECT_EQ(FileGDBIterator::Build(&table, 2, TRUE, FGSO_EQ, OFTString,
                                       &long_fld),
                nullptr);
    }

    // Missing .atx file error in ReadTrailer
    {
      const std::string atx_path =
          std::string(kTestDir) + "/errors.idx_int.atx";
      VSIUnlink(atx_path.c_str());
      WithQuietHandler quiet;
      EXPECT_EQ(FileGDBIterator::Build(&table, 1, TRUE, FGSO_ISNOTNULL,
                                       OFTMaxType, nullptr),
                nullptr);
    }
  }

  // 4. GUID index test via synthetic .atx and .gdbindexes
  {
    // Generate valid .gdbindexes template using temporary table with string
    // field
    const std::string tmp_path = std::string(kTestDir) + "/tmp_guid.gdbtable";
    {
      FileGDBTable tmp;
      ASSERT_TRUE(tmp.Create(tmp_path.c_str(), 4, FGTGT_NONE, false, false));
      ASSERT_TRUE(tmp.CreateField(std::make_unique<FileGDBField>(
          "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
          FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(tmp.CreateField(std::make_unique<FileGDBField>(
          "uuid_val", "", FGFT_STRING, true, false, true, 0,
          FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(tmp.Sync());
      ASSERT_TRUE(tmp.CreateIndex("idx_uuid", "uuid_val"));
      ASSERT_TRUE(tmp.Sync());
    }

    // Create guid_tbl with FGFT_GUID
    const std::string guid_path = std::string(kTestDir) + "/guid_tbl.gdbtable";
    {
      FileGDBTable table;
      ASSERT_TRUE(table.Create(guid_path.c_str(), 4, FGTGT_NONE, false, false));
      ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
          "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
          FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(table.CreateField(
          std::make_unique<FileGDBField>("uuid_val", "", FGFT_GUID, true, false,
                                         true, 0, FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(table.Sync());

      std::vector<OGRField> flds = MakeUnsetFields(table.GetFieldCount());
      char u1[] = "{11111111-1111-1111-1111-111111111111}";
      flds[1].String = u1;
      ASSERT_TRUE(table.CreateFeature(flds, nullptr));
      char u2[] = "{22222222-2222-2222-2222-222222222222}";
      flds[1].String = u2;
      ASSERT_TRUE(table.CreateFeature(flds, nullptr));
      ASSERT_TRUE(table.Sync());
    }

    // Move .gdbindexes to guid_tbl.gdbindexes
    const std::string tmp_idx = std::string(kTestDir) + "/tmp_guid.gdbindexes";
    const std::string guid_idx = std::string(kTestDir) + "/guid_tbl.gdbindexes";
    ASSERT_EQ(VSIRename(tmp_idx.c_str(), guid_idx.c_str()), 0);

    // Write 1-page V1 .atx file (4096 + 22 bytes)
    const std::string atx_path =
        std::string(kTestDir) + "/guid_tbl.idx_uuid.atx";
    VSILFILE* fp_atx = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp_atx, nullptr);
    std::vector<GByte> atx_buf(4096 + 22, 0);
    // Page 1 (leaf): nFeatures = 2 at offset 4
    uint32_t nFeat = 2;
    memcpy(atx_buf.data() + 4, &nFeat, 4);
    uint32_t fid1 = 1, fid2 = 2;
    memcpy(atx_buf.data() + 12, &fid1, 4);
    memcpy(atx_buf.data() + 16, &fid2, 4);
    // Values offset = 12 + 97 * 4 = 400
    int offset_val = 400;
    memcpy(atx_buf.data() + offset_val,
           "{11111111-1111-1111-1111-111111111111}", 38);
    memcpy(atx_buf.data() + offset_val + 38,
           "{22222222-2222-2222-2222-222222222222}", 38);
    // Trailer (22 bytes at end)
    int tr_offset = 4096;
    atx_buf[tr_offset] = 38;  // m_nValueSize
    atx_buf[tr_offset + 1] = 0x40;
    uint32_t mg = 1, dp = 1, count = 2, vr = 1;
    memcpy(atx_buf.data() + tr_offset + 2, &mg, 4);
    memcpy(atx_buf.data() + tr_offset + 6, &dp, 4);
    memcpy(atx_buf.data() + tr_offset + 10, &count, 4);
    memcpy(atx_buf.data() + tr_offset + 18, &vr, 4);
    VSIFWriteL(atx_buf.data(), 1, atx_buf.size(), fp_atx);
    VSIFCloseL(fp_atx);

    // Open table and test GUID iterator
    FileGDBTable table;
    ASSERT_TRUE(table.Open(guid_path.c_str(), false));
    ASSERT_EQ(table.GetIndexCount(), 1);

    // IS NOT NULL
    auto iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
        &table, 1, TRUE, FGSO_ISNOTNULL, OFTMaxType, nullptr));
    ASSERT_NE(iter, nullptr);
    EXPECT_EQ(iter->GetRowCount(), 2);
    EXPECT_EQ(iter->GetNextRowSortedByFID(), 0);
    EXPECT_EQ(iter->GetNextRowSortedByFID(), 1);
    EXPECT_EQ(iter->GetNextRowSortedByFID(), -1);

    int eOutType = -1;
    const OGRField* min_fld = iter->GetMinValue(eOutType);
    ASSERT_NE(min_fld, nullptr);
    EXPECT_STREQ(min_fld->String, "{11111111-1111-1111-1111-111111111111}");
    const OGRField* max_fld = iter->GetMaxValue(eOutType);
    ASSERT_NE(max_fld, nullptr);
    EXPECT_STREQ(max_fld->String, "{22222222-2222-2222-2222-222222222222}");

    // EQ with short string (bEvaluateToFALSE)
    {
      OGRField f;
      char short_str[] = "short";
      f.String = short_str;
      auto eq_iter = std::unique_ptr<FileGDBIterator>(
          FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTString, &f));
      ASSERT_NE(eq_iter, nullptr);
      EXPECT_EQ(eq_iter->GetRowCount(), 0);
      EXPECT_EQ(eq_iter->GetNextRowSortedByValue(), -1);
    }

    // EQ matching first row
    {
      OGRField f;
      char match_str[] = "{11111111-1111-1111-1111-111111111111}";
      f.String = match_str;
      auto eq_iter = std::unique_ptr<FileGDBIterator>(
          FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTString, &f));
      ASSERT_NE(eq_iter, nullptr);
      EXPECT_EQ(eq_iter->GetNextRowSortedByFID(), 0);
      EXPECT_EQ(eq_iter->GetNextRowSortedByFID(), -1);
    }

    // LT, LE, GT, GE
    {
      OGRField f;
      char mid_str[] = "{20000000-0000-0000-0000-000000000000}";
      f.String = mid_str;
      auto lt_iter = std::unique_ptr<FileGDBIterator>(
          FileGDBIterator::Build(&table, 1, TRUE, FGSO_LT, OFTString, &f));
      ASSERT_NE(lt_iter, nullptr);
      EXPECT_EQ(lt_iter->GetNextRowSortedByFID(), 0);

      auto gt_iter = std::unique_ptr<FileGDBIterator>(
          FileGDBIterator::Build(&table, 1, TRUE, FGSO_GT, OFTString, &f));
      ASSERT_NE(gt_iter, nullptr);
      EXPECT_EQ(gt_iter->GetNextRowSortedByFID(), 1);
    }
  }
}

// -----------------------------------------------------------------------------
// Suite 2: Composite & wrapper iterators (Trivial, Not, And, Or)
// -----------------------------------------------------------------------------
TEST_F(FileGDBIndexTest, CompositeAndWrapperIterators) {
  const std::string path = std::string(kTestDir) + "/composite.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(path.c_str(), 4, FGTGT_NONE, false, false));
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "f1", "", FGFT_INT32, true, false, true, 0, FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "f2", "", FGFT_INT32, true, false, true, 0, FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table.Sync());

  // Insert 10 rows: f1 = i * 10, f2 = (10 - i) * 10
  for (int i = 0; i < 10; ++i) {
    auto flds = MakeUnsetFields(table.GetFieldCount());
    flds[1].Integer = i * 10;
    flds[2].Integer = (10 - i) * 10;
    ASSERT_TRUE(table.CreateFeature(flds, nullptr));
  }
  ASSERT_TRUE(table.Sync());
  ASSERT_TRUE(table.CreateIndex("idx_f1", "f1"));
  ASSERT_TRUE(table.CreateIndex("idx_f2", "f2"));

  // 1. FileGDBTrivialIterator (returned by BuildIsNotNull when all rows
  // non-null)
  {
    auto iter = std::unique_ptr<FileGDBIterator>(
        FileGDBIterator::BuildIsNotNull(&table, 1, TRUE));
    ASSERT_NE(iter, nullptr);
    EXPECT_EQ(iter->GetTable(), &table);
    EXPECT_EQ(iter->GetRowCount(), 10);

    for (int64_t expected_row = 0; expected_row < 10; ++expected_row) {
      EXPECT_EQ(iter->GetNextRowSortedByFID(), expected_row);
    }
    EXPECT_EQ(iter->GetNextRowSortedByFID(), -1);

    iter->Reset();
    EXPECT_GE(iter->GetNextRowSortedByValue(), 0);

    int eOutType = -1;
    const OGRField* min_val = iter->GetMinValue(eOutType);
    EXPECT_NE(min_val, nullptr);
    EXPECT_EQ(min_val->Integer, 0);

    const OGRField* max_val = iter->GetMaxValue(eOutType);
    EXPECT_NE(max_val, nullptr);
    EXPECT_EQ(max_val->Integer, 90);

    double dfMin = 0, dfMax = 0, dfSum = 0;
    int nCount = 0;
    EXPECT_TRUE(iter->GetMinMaxSumCount(dfMin, dfMax, dfSum, nCount));
    EXPECT_EQ(nCount, 10);
    EXPECT_EQ(dfMin, 0.0);
    EXPECT_EQ(dfMax, 90.0);
  }

  // 2. FileGDBNotIterator
  // 2a. Table without holes (bNoHoles == true)
  {
    OGRField val;
    val.Integer = 50;
    auto base_iter =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &val);
    ASSERT_NE(base_iter, nullptr);

    auto not_iter =
        std::unique_ptr<FileGDBIterator>(FileGDBIterator::BuildNot(base_iter));
    ASSERT_NE(not_iter, nullptr);
    EXPECT_EQ(not_iter->GetTable(), &table);
    EXPECT_EQ(not_iter->GetRowCount(), 9);

    int count = 0;
    while (not_iter->GetNextRowSortedByFID() >= 0) count++;
    EXPECT_EQ(count, 9);

    not_iter->Reset();
    EXPECT_GE(not_iter->GetNextRowSortedByFID(), 0);
  }

  // 2b. Table with holes (delete row 3, so FID 4 is a hole)
  {
    const std::string holes_path =
        std::string(kTestDir) + "/composite_holes.gdbtable";
    FileGDBTable holes_table;
    ASSERT_TRUE(
        holes_table.Create(holes_path.c_str(), 4, FGTGT_NONE, false, false));
    ASSERT_TRUE(holes_table.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(holes_table.CreateField(
        std::make_unique<FileGDBField>("f1", "", FGFT_INT32, true, false, true,
                                       0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(holes_table.Sync());

    for (int i = 0; i < 10; ++i) {
      auto flds = MakeUnsetFields(holes_table.GetFieldCount());
      flds[1].Integer = i * 10;
      ASSERT_TRUE(holes_table.CreateFeature(flds, nullptr));
    }
    ASSERT_TRUE(holes_table.Sync());
    ASSERT_TRUE(holes_table.DeleteFeature(4));
    ASSERT_TRUE(holes_table.Sync());
    ASSERT_TRUE(holes_table.CreateIndex("idx_h1", "f1"));

    OGRField val;
    val.Integer = 20;
    auto base_iter = FileGDBIterator::Build(&holes_table, 1, TRUE, FGSO_EQ,
                                            OFTInteger, &val);
    ASSERT_NE(base_iter, nullptr);

    auto not_iter =
        std::unique_ptr<FileGDBIterator>(FileGDBIterator::BuildNot(base_iter));
    ASSERT_NE(not_iter, nullptr);
    EXPECT_EQ(not_iter->GetRowCount(), 8);

    int count = 0;
    while (not_iter->GetNextRowSortedByFID() >= 0) count++;
    EXPECT_EQ(count, 8);
  }

  // 3. FileGDBAndIterator
  {
    OGRField v1, v2;
    v1.Integer = 20;
    v2.Integer = 70;
    auto it1 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_GE, OFTInteger, &v1);
    auto it2 =
        FileGDBIterator::Build(&table, 2, TRUE, FGSO_LE, OFTInteger, &v2);
    ASSERT_NE(it1, nullptr);
    ASSERT_NE(it2, nullptr);

    auto and_iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::BuildAnd(
        it1, it2, /*bTakeOwnershipOfIterators=*/true));
    ASSERT_NE(and_iter, nullptr);
    EXPECT_EQ(and_iter->GetTable(), &table);

    int count = 0;
    while (and_iter->GetNextRowSortedByFID() >= 0) count++;
    EXPECT_GT(count, 0);

    and_iter->Reset();
    EXPECT_EQ(and_iter->GetRowCount(), count);

    // Call base class methods that print error and return defaults
    {
      WithQuietHandler quiet;
      int out_type = 0;
      EXPECT_EQ(and_iter->GetMinValue(out_type), nullptr);
      EXPECT_EQ(and_iter->GetMaxValue(out_type), nullptr);
      EXPECT_EQ(and_iter->GetNextRowSortedByValue(), -1);
      double min = 0, max = 0, sum = 0;
      int cnt = 0;
      EXPECT_FALSE(and_iter->GetMinMaxSumCount(min, max, sum, cnt));
    }
  }

  // FileGDBAndIterator without ownership
  {
    OGRField v1, v2;
    v1.Integer = 20;
    v2.Integer = 70;
    auto it1 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_GE, OFTInteger, &v1);
    auto it2 =
        FileGDBIterator::Build(&table, 2, TRUE, FGSO_LE, OFTInteger, &v2);
    {
      FileGDBIterator* and_iter = FileGDBIterator::BuildAnd(
          it1, it2, /*bTakeOwnershipOfIterators=*/false);
      EXPECT_GE(and_iter->GetNextRowSortedByFID(), 0);
      delete and_iter;
    }
    delete it1;
    delete it2;
  }

  // 4. FileGDBOrIterator
  // 4a. Non-exclusive OR
  {
    OGRField v1, v2;
    v1.Integer = 20;
    v2.Integer = 60;
    auto it1 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &v1);
    auto it2 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &v2);
    auto or_iter = std::unique_ptr<FileGDBIterator>(
        FileGDBIterator::BuildOr(it1, it2, /*bIteratorAreExclusive=*/FALSE));
    ASSERT_NE(or_iter, nullptr);
    EXPECT_EQ(or_iter->GetTable(), &table);
    EXPECT_EQ(or_iter->GetRowCount(), 2);

    int count = 0;
    while (or_iter->GetNextRowSortedByFID() >= 0) count++;
    EXPECT_EQ(count, 2);
  }

  // 4b. Exclusive OR with disjoint sets
  {
    OGRField v1, v2;
    v1.Integer = 10;
    v2.Integer = 80;
    auto it1 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &v1);
    auto it2 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &v2);
    auto or_iter = std::unique_ptr<FileGDBIterator>(
        FileGDBIterator::BuildOr(it1, it2, /*bIteratorAreExclusive=*/TRUE));
    ASSERT_NE(or_iter, nullptr);
    EXPECT_EQ(or_iter->GetRowCount(), 2);
  }

  // 4c. Exclusive OR on overlapping sets triggers PrintError
  {
    OGRField v1, v2;
    v1.Integer = 10;
    v2.Integer = 10;
    auto it1 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &v1);
    auto it2 =
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &v2);
    auto or_iter = std::unique_ptr<FileGDBIterator>(
        FileGDBIterator::BuildOr(it1, it2, /*bIteratorAreExclusive=*/TRUE));
    ASSERT_NE(or_iter, nullptr);
    WithQuietHandler quiet;
    EXPECT_GE(or_iter->GetNextRowSortedByFID(), 0);
  }
}

// -----------------------------------------------------------------------------
// Suite 3: All field types, operators, depth 2 and aggregates
// -----------------------------------------------------------------------------
TEST_F(FileGDBIndexTest, AllFieldTypesOperatorsDepth2AndAggregates) {
  const std::string path = std::string(kTestDir) + "/all_types.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(path.c_str(), 4, FGTGT_NONE, false, false));

  // Field 0: OBJECTID
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD)));

  // Field 1: INT16
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_int16", "", FGFT_INT16, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 2: INT32
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_int32", "", FGFT_INT32, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 3: INT64
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_int64", "", FGFT_INT64, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 4: FLOAT32
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_float32", "", FGFT_FLOAT32, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 5: FLOAT64
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_float64", "", FGFT_FLOAT64, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 6: STRING (normal index)
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_str", "", FGFT_STRING, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 7: STRING (lowercase index)
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_str_low", "", FGFT_STRING, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // Field 8: DATETIME
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "f_datetime", "", FGFT_DATETIME, true, false, true, 0,
      FileGDBField::UNSET_FIELD)));

  // Field 9: DATE
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_date", "", FGFT_DATE, true, false, true,
                                     0, FileGDBField::UNSET_FIELD)));

  // Field 10: TIME
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("f_time", "", FGFT_TIME, true, false, true,
                                     0, FileGDBField::UNSET_FIELD)));

  // Field 11: DATETIME_WITH_OFFSET
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "f_dto", "", FGFT_DATETIME_WITH_OFFSET, true, false, true, 0,
      FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(table.Sync());

  // Populate 700 rows: > 680 (nMaxPerPages for INT16), ensuring nIndexDepth = 2
  for (int i = 1; i <= 700; ++i) {
    auto flds = MakeUnsetFields(table.GetFieldCount());
    flds[1].Integer = i;
    flds[2].Integer = i * 10;
    flds[3].Integer64 = static_cast<int64_t>(i) * 1000000LL;
    flds[4].Real = static_cast<float>(i * 0.5f);
    flds[5].Real = static_cast<double>(i) * 1.25;

    std::string s_val = CPLSPrintf("Item_%04d", i);
    flds[6].String = const_cast<char*>(s_val.c_str());

    std::string s_low = CPLSPrintf("item%04d", i);
    flds[7].String = const_cast<char*>(s_low.c_str());

    std::memset(&flds[8], 0, sizeof(OGRField));
    flds[8].Date.Year = 2000 + (i % 25);
    flds[8].Date.Month = 1 + (i % 12);
    flds[8].Date.Day = 1 + (i % 28);
    flds[8].Date.Hour = (i % 24);
    flds[8].Date.Minute = (i % 60);
    flds[8].Date.Second = static_cast<float>(i % 60);

    std::memset(&flds[9], 0, sizeof(OGRField));
    flds[9].Date.Year = 2000 + (i % 25);
    flds[9].Date.Month = 1 + (i % 12);
    flds[9].Date.Day = 1 + (i % 28);

    std::memset(&flds[10], 0, sizeof(OGRField));
    flds[10].Date.Hour = (i % 24);
    flds[10].Date.Minute = (i % 60);
    flds[10].Date.Second = static_cast<float>(i % 60);

    std::memset(&flds[11], 0, sizeof(OGRField));
    flds[11].Date.Year = 2000 + (i % 25);
    flds[11].Date.Month = 1 + (i % 12);
    flds[11].Date.Day = 1 + (i % 28);
    flds[11].Date.Hour = (i % 24);
    flds[11].Date.Minute = (i % 60);
    flds[11].Date.Second = static_cast<float>(i % 60);
    flds[11].Date.TZFlag = 100;

    ASSERT_TRUE(table.CreateFeature(flds, nullptr));
  }

  // Row 701: row with null fields so BuildIsNotNull is non-trivial
  {
    auto flds = MakeUnsetFields(table.GetFieldCount());
    ASSERT_TRUE(table.CreateFeature(flds, nullptr));
  }
  ASSERT_TRUE(table.Sync());

  // Create indexes on all fields
  ASSERT_TRUE(table.CreateIndex("idx_1", "f_int16"));
  ASSERT_TRUE(table.CreateIndex("idx_2", "f_int32"));
  ASSERT_TRUE(table.CreateIndex("idx_3", "f_int64"));
  ASSERT_TRUE(table.CreateIndex("idx_4", "f_float32"));
  ASSERT_TRUE(table.CreateIndex("idx_5", "f_float64"));
  ASSERT_TRUE(table.CreateIndex("idx_6", "f_str"));
  ASSERT_TRUE(table.CreateIndex("idx_7", "LOWER(f_str_low)"));
  ASSERT_TRUE(table.CreateIndex("idx_8", "f_datetime"));
  ASSERT_TRUE(table.CreateIndex("idx_9", "f_date"));
  ASSERT_TRUE(table.CreateIndex("idx_10", "f_time"));
  ASSERT_TRUE(table.CreateIndex("idx_11", "f_dto"));

  // Helper to test all operators and both sorting orders on a given field
  auto test_field_ops = [&](int field_idx, OGRFieldType ogr_type,
                            const std::vector<OGRField>& test_vals,
                            bool is_numeric) {
    // 1. IS NOT NULL (ascending and descending)
    for (int bAsc : {TRUE, FALSE}) {
      auto iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
          &table, field_idx, bAsc, FGSO_ISNOTNULL, OFTMaxType, nullptr));
      ASSERT_NE(iter, nullptr);
      EXPECT_EQ(iter->GetRowCount(), 700);

      // Consume with GetNextRowSortedByValue
      int count_val = 0;
      while (iter->GetNextRowSortedByValue() >= 0) count_val++;
      EXPECT_EQ(count_val, 700);

      // Consume with GetNextRowSortedByFID
      iter->Reset();
      int count_fid = 0;
      while (iter->GetNextRowSortedByFID() >= 0) count_fid++;
      EXPECT_EQ(count_fid, 700);

      // GetRowCount when sorted rows cache is active
      EXPECT_EQ(iter->GetRowCount(), 700);

      // Min/Max
      int eOutType = -1;
      const OGRField* pMin = iter->GetMinValue(eOutType);
      EXPECT_NE(pMin, nullptr);
      const OGRField* pMax = iter->GetMaxValue(eOutType);
      EXPECT_NE(pMax, nullptr);

      // Aggregates (numeric only)
      double dfMin = 0, dfMax = 0, dfSum = 0;
      int nAggrCount = 0;
      bool aggr_ret = iter->GetMinMaxSumCount(dfMin, dfMax, dfSum, nAggrCount);
      if (is_numeric) {
        EXPECT_TRUE(aggr_ret);
        EXPECT_EQ(nAggrCount, 700);
      } else {
        EXPECT_FALSE(aggr_ret);
      }
    }

    // 2. Relational operators
    std::vector<FileGDBSQLOp> ops = {FGSO_LT, FGSO_LE, FGSO_EQ, FGSO_GE,
                                     FGSO_GT};
    for (const auto& val : test_vals) {
      for (auto op : ops) {
        for (int bAsc : {TRUE, FALSE}) {
          auto iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
              &table, field_idx, bAsc, op, ogr_type, &val));
          ASSERT_NE(iter, nullptr);
          iter->GetRowCount();

          while (iter->GetNextRowSortedByValue() >= 0) {
          }
          iter->Reset();
          while (iter->GetNextRowSortedByFID() >= 0) {
          }
        }
      }
    }
  };

  // Test INT16
  {
    std::vector<OGRField> vals(2);
    vals[0].Integer = 100;
    vals[1].Integer = 350;
    test_field_ops(1, OFTInteger, vals, true);
  }

  // Test INT32
  {
    std::vector<OGRField> vals(2);
    vals[0].Integer = 1000;
    vals[1].Integer = 3500;
    test_field_ops(2, OFTInteger, vals, true);
  }

  // Test INT64
  {
    std::vector<OGRField> vals(2);
    vals[0].Integer64 = 100000000LL;
    vals[1].Integer64 = 350000000LL;
    test_field_ops(3, OFTInteger64, vals, true);
  }

  // Test FLOAT32
  {
    std::vector<OGRField> vals(2);
    vals[0].Real = 50.0;
    vals[1].Real = 175.0;
    test_field_ops(4, OFTReal, vals, true);
  }

  // Test FLOAT64
  {
    std::vector<OGRField> vals(2);
    vals[0].Real = 125.0;
    vals[1].Real = 437.5;
    test_field_ops(5, OFTReal, vals, true);
  }

  // Test STRING normal
  {
    std::vector<OGRField> vals(2);
    char s1[] = "Item_0100";
    char s2[] = "Item_0350";
    vals[0].String = s1;
    vals[1].String = s2;
    test_field_ops(6, OFTString, vals, false);
  }

  // Test STRING lowercase with ILIKE
  {
    std::vector<OGRField> vals(1);
    char s1[] = "item0100";
    vals[0].String = s1;
    for (int bAsc : {TRUE, FALSE}) {
      auto iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
          &table, 7, bAsc, FGSO_ILIKE, OFTString, &vals[0]));
      ASSERT_NE(iter, nullptr);
      EXPECT_EQ(iter->GetRowCount(), 1);
      EXPECT_GE(iter->GetNextRowSortedByValue(), 0);
    }

    char s2[] = "ITEM0100";
    vals[0].String = s2;
    auto iter2 = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
        &table, 7, TRUE, FGSO_ILIKE, OFTString, &vals[0]));
    ASSERT_NE(iter2, nullptr);
    EXPECT_EQ(iter2->GetRowCount(), 1);
  }

  // Test DATETIME with both OFTDateTime and OFTReal
  {
    std::vector<OGRField> vals(2);
    std::memset(&vals[0], 0, sizeof(OGRField));
    vals[0].Date.Year = 2010;
    vals[0].Date.Month = 6;
    vals[0].Date.Day = 15;
    vals[1].Real = 40000.0;
    test_field_ops(8, OFTDateTime, {vals[0]}, true);
    test_field_ops(8, OFTReal, {vals[1]}, true);
  }

  // Test DATE with both OFTDate and OFTReal
  {
    std::vector<OGRField> vals(2);
    std::memset(&vals[0], 0, sizeof(OGRField));
    vals[0].Date.Year = 2010;
    vals[0].Date.Month = 6;
    vals[0].Date.Day = 15;
    vals[1].Real = 40000.0;
    test_field_ops(9, OFTDate, {vals[0]}, true);
    test_field_ops(9, OFTReal, {vals[1]}, true);
  }

  // Test TIME with both OFTTime and OFTReal
  {
    std::vector<OGRField> vals(2);
    std::memset(&vals[0], 0, sizeof(OGRField));
    vals[0].Date.Hour = 12;
    vals[0].Date.Minute = 30;
    vals[0].Date.Second = 0.0f;
    vals[1].Real = 45000.0;
    test_field_ops(10, OFTTime, {vals[0]}, true);
    test_field_ops(10, OFTReal, {vals[1]}, true);
  }

  // Test DATETIME_WITH_OFFSET
  {
    std::vector<OGRField> vals(1);
    std::memset(&vals[0], 0, sizeof(OGRField));
    vals[0].Date.Year = 2010;
    vals[0].Date.Month = 6;
    vals[0].Date.Day = 15;
    vals[0].Date.TZFlag = 100;
    test_field_ops(11, OFTDateTime, vals, true);
  }

  // Test Depth 3 index navigation
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE", "4");
    auto cleanup = absl::MakeCleanup([] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE",
                                    nullptr);
    });

    const std::string d3_path = std::string(kTestDir) + "/depth3.gdbtable";
    FileGDBTable d3_table;
    ASSERT_TRUE(d3_table.Create(d3_path.c_str(), 4, FGTGT_NONE, false, false));
    ASSERT_TRUE(d3_table.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(d3_table.CreateField(
        std::make_unique<FileGDBField>("s", "", FGFT_STRING, true, false, true,
                                       0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(d3_table.Sync());

    for (int i = 1; i <= 30; ++i) {
      auto f = MakeUnsetFields(d3_table.GetFieldCount());
      std::string s(80, 'A' + (i % 26));
      f[1].String = const_cast<char*>(s.c_str());
      ASSERT_TRUE(d3_table.CreateFeature(f, nullptr));
    }
    ASSERT_TRUE(d3_table.Sync());
    ASSERT_TRUE(d3_table.CreateIndex("idx_d3", "s"));

    for (int bAsc : {TRUE, FALSE}) {
      auto iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
          &d3_table, 1, bAsc, FGSO_ISNOTNULL, OFTMaxType, nullptr));
      ASSERT_NE(iter, nullptr);
      EXPECT_EQ(iter->GetRowCount(), 30);
      while (iter->GetNextRowSortedByValue() >= 0) {
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Suite 4: Spatial index iterator all grid levels, depths, and edge cases
// -----------------------------------------------------------------------------
TEST_F(FileGDBIndexTest, SpatialIndexIteratorAllGridLevelsAndDepths) {
  CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE", "15");
  auto cleanup = absl::MakeCleanup([] {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_MAX_FEATURES_PER_SPX_PAGE",
                                  nullptr);
  });

  const std::string path = std::string(kTestDir) + "/spatial_idx.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(path.c_str(), 4, FGTGT_LINE, false, false));
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD)));

  std::vector<double> grid_res = {10.0};
  auto geom_field = std::make_unique<FileGDBGeomField>(
      "Shape", "", true, "LINESTRING", 0.0, 0.0, 10000.0, 0.001, grid_res);
  ASSERT_TRUE(table.CreateField(std::move(geom_field)));
  ASSERT_TRUE(table.Sync());

  // 360 lines ensures m_nValueCountInIdx >= 340 (nMaxPerPages) with depth 3
  for (int i = 1; i <= 360; ++i) {
    auto flds = MakeUnsetFields(table.GetFieldCount());
    OGRLineString ls;
    double x0 = (i % 30) * 100.0;
    double y0 = (i / 30) * 100.0;
    ls.addPoint(x0, y0);
    ls.addPoint(x0 + 5.0, y0 + 5.0);
    ASSERT_TRUE(table.CreateFeature(flds, &ls));
  }
  ASSERT_TRUE(table.Sync());
  ASSERT_TRUE(table.CreateSpatialIndex());

  // Test FileGDBSpatialIndexIterator::Build
  OGREnvelope env;
  env.MinX = 0.0;
  env.MaxX = 800.0;
  env.MinY = 0.0;
  env.MaxY = 400.0;

  auto sp_iter = std::unique_ptr<FileGDBSpatialIndexIterator>(
      FileGDBSpatialIndexIterator::Build(&table, env));
  ASSERT_NE(sp_iter, nullptr);
  EXPECT_EQ(sp_iter->GetTable(), &table);

  int count = 0;
  while (sp_iter->GetNextRowSortedByFID() >= 0) count++;
  EXPECT_GT(count, 0);

  // SetEnvelope
  OGREnvelope env2;
  env2.MinX = 200.0;
  env2.MaxX = 500.0;
  env2.MinY = 100.0;
  env2.MaxY = 300.0;
  EXPECT_TRUE(sp_iter->SetEnvelope(env2));

  int count2 = 0;
  while (sp_iter->GetNextRowSortedByFID() >= 0) count2++;
  EXPECT_GT(count2, 0);

  // Query envelope completely below table extent (triggers GetInt64 >
  // m_nMaxVal)
  OGREnvelope env_below;
  env_below.MinX = -2000.0;
  env_below.MaxX = -1000.0;
  env_below.MinY = -2000.0;
  env_below.MaxY = -1000.0;
  EXPECT_TRUE(sp_iter->SetEnvelope(env_below));
  EXPECT_EQ(sp_iter->GetNextRowSortedByFID(), -1);

  // Query envelope completely above table extent (triggers FindMinMaxIdx
  // returning false)
  OGREnvelope env_above;
  env_above.MinX = 50000.0;
  env_above.MaxX = 60000.0;
  env_above.MinY = 50000.0;
  env_above.MaxY = 60000.0;
  EXPECT_TRUE(sp_iter->SetEnvelope(env_above));
  EXPECT_EQ(sp_iter->GetNextRowSortedByFID(), -1);

  // Spatial Index Error cases
  // 1. Missing .spx file
  {
    const std::string spx_path = std::string(kTestDir) + "/spatial_idx.spx";
    std::string backup_spx = spx_path + ".bak";
    VSIRename(spx_path.c_str(), backup_spx.c_str());
    WithQuietHandler quiet;
    auto iter = std::unique_ptr<FileGDBSpatialIndexIterator>(
        FileGDBSpatialIndexIterator::Build(&table, env));
    EXPECT_EQ(iter, nullptr);
    VSIRename(backup_spx.c_str(), spx_path.c_str());
  }

  // 2. Suspicious nIndexDepth == 1 .spx file (lines 2535-2564)
  {
    const std::string bad_spx_table =
        std::string(kTestDir) + "/suspicious_spx.gdbtable";
    FileGDBTable t;
    ASSERT_TRUE(t.Create(bad_spx_table.c_str(), 4, FGTGT_POINT, false, false));
    ASSERT_TRUE(t.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    std::vector<double> r = {10.0};
    auto gf = std::make_unique<FileGDBGeomField>("Shape", "", true, "POINT",
                                                 0.0, 0.0, 10000.0, 0.001, r);
    ASSERT_TRUE(t.CreateField(std::move(gf)));
    ASSERT_TRUE(t.Sync());

    for (int i = 0; i < 40; ++i) {
      auto f = MakeUnsetFields(t.GetFieldCount());
      OGRPoint pt(100.0 + i, 100.0 + i);
      ASSERT_TRUE(t.CreateFeature(f, &pt));
    }
    ASSERT_TRUE(t.Sync());

    // Craft a 3-page .spx file where nIndexDepth = 1, page 1 has 2 IDs (2 and
    // 3)
    const std::string bad_spx_path =
        std::string(kTestDir) + "/suspicious_spx.spx";
    VSILFILE* fp = VSIFOpenL(bad_spx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::vector<GByte> page(4096, 0);

    // Page 1: leaf page header (12 bytes), nFeatures = 2 at offset 4
    uint32_t nFeat = 2;
    memcpy(page.data() + 4, &nFeat, 4);
    // Two IDs: 2 and 3 at offset 8 and 12 (abyPageFeature + 8)
    uint32_t id1 = 2, id2 = 3;
    memcpy(page.data() + 8, &id1, 4);
    memcpy(page.data() + 12, &id2, 4);
    VSIFWriteL(page.data(), 1, 4096, fp);
    // Page 2 & 3
    VSIFWriteL(page.data(), 1, 4096, fp);
    VSIFWriteL(page.data(), 1, 4096, fp);

    // Trailer (22 bytes): valueSize = 8, magic = 1, depth = 1, valCount = 2,
    // version = 1
    std::vector<GByte> tr(22, 0);
    tr[0] = 8;
    uint32_t mg = 1, dp = 1, vc = 2, vr = 1;
    memcpy(tr.data() + 2, &mg, 4);
    memcpy(tr.data() + 6, &dp, 4);
    memcpy(tr.data() + 10, &vc, 4);
    memcpy(tr.data() + 18, &vr, 4);
    VSIFWriteL(tr.data(), 1, 22, fp);
    VSIFCloseL(fp);

    WithQuietHandler quiet;
    auto iter = std::unique_ptr<FileGDBSpatialIndexIterator>(
        FileGDBSpatialIndexIterator::Build(&t, env));
    EXPECT_EQ(iter, nullptr);
    VSIUnlink(bad_spx_path.c_str());
  }
}

// -----------------------------------------------------------------------------
// Suite 5: Version 2 format attribute and spatial index tests
// -----------------------------------------------------------------------------
TEST_F(FileGDBIndexTest, Version2AttributeAndSpatialIndexes) {
  const std::string path = std::string(kTestDir) + "/v2_test.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(path.c_str(), 4, FGTGT_POINT, false, false));
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD)));
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("v2_int", "", FGFT_INT32, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));
  std::vector<double> r = {10.0};
  auto gf = std::make_unique<FileGDBGeomField>("Shape", "", true, "POINT", 0.0,
                                               0.0, 10000.0, 0.001, r);
  ASSERT_TRUE(table.CreateField(std::move(gf)));
  ASSERT_TRUE(table.Sync());

  // Insert 2 rows
  for (int i = 1; i <= 2; ++i) {
    auto flds = MakeUnsetFields(table.GetFieldCount());
    flds[1].Integer = i * 100;
    OGRPoint pt(100.0 * i, 100.0 * i);
    ASSERT_TRUE(table.CreateFeature(flds, &pt));
  }
  ASSERT_TRUE(table.Sync());

  // 1. Construct synthetic V2 .atx file:
  // - Page size 65536, trailer size 30, version = 2, nIndexDepth = 2
  // - Root page 1 points to leaf page 2
  // - Leaf page 2 has 2 features
  {
    table.CreateIndex("v2_idx", "v2_int");
    const std::string atx_path = std::string(kTestDir) + "/v2_test.v2_idx.atx";
    VSILFILE* fp = VSIFOpenL(atx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);

    // Page 1: root page (non-leaf header = 12 bytes: objectIDSize=8,
    // nSubPages=1)
    std::vector<GByte> p1(65536, 0);
    uint32_t nSubPages = 1;
    memcpy(p1.data() + 8, &nSubPages, 4);
    uint64_t child_page = 2;
    memcpy(p1.data() + 12, &child_page, 8);  // points to page 2 (min)
    memcpy(p1.data() + 20, &child_page, 8);  // points to page 2 (max)
    int32_t max_val = 200;
    // max per pages = (65536 - 20) / (8 + 4) = 5459
    // offset first val = 20 + 5459 * 8 = 43692
    int offset_val_p1 = 20 + ((65536 - 20) / (8 + 4)) * 8;
    memcpy(p1.data() + offset_val_p1, &max_val, 4);
    VSIFWriteL(p1.data(), 1, 65536, fp);

    // Page 2: leaf page (header = 20 bytes: nFeatures=2 at offset 8)
    std::vector<GByte> p2(65536, 0);
    uint32_t nFeatures = 2;
    memcpy(p2.data() + 8, &nFeatures, 4);
    uint64_t fid1 = 1, fid2 = 2;
    memcpy(p2.data() + 20, &fid1, 8);
    memcpy(p2.data() + 28, &fid2, 8);
    int32_t v1 = 100, v2 = 200;
    int offset_val_p2 = offset_val_p1;
    memcpy(p2.data() + offset_val_p2, &v1, 4);
    memcpy(p2.data() + offset_val_p2 + 4, &v2, 4);
    VSIFWriteL(p2.data(), 1, 65536, fp);

    // Trailer (30 bytes):
    // [0]: valueSize = 4
    // [2..5]: magic = 1
    // [6..9]: depth = 2
    // [10..17]: count = 2
    // [26..29]: version = 2
    std::vector<GByte> tr(30, 0);
    tr[0] = 4;
    uint32_t mg = 1, dp = 2, vr = 2;
    uint64_t vc = 2;
    memcpy(tr.data() + 2, &mg, 4);
    memcpy(tr.data() + 6, &dp, 4);
    memcpy(tr.data() + 10, &vc, 8);
    memcpy(tr.data() + 26, &vr, 4);
    VSIFWriteL(tr.data(), 1, 30, fp);
    VSIFCloseL(fp);

    // Test FileGDBIndex::GetMaxWidthInBytes on V2
    const FileGDBIndex* v2_idx = table.GetField(1)->GetIndex();
    ASSERT_NE(v2_idx, nullptr);
    EXPECT_EQ(v2_idx->GetMaxWidthInBytes(&table), 4);

    // Build V2 iterator with IS NOT NULL
    auto iter = std::unique_ptr<FileGDBIterator>(FileGDBIterator::Build(
        &table, 1, TRUE, FGSO_ISNOTNULL, OFTMaxType, nullptr));
    ASSERT_NE(iter, nullptr);
    EXPECT_EQ(iter->GetRowCount(), 2);

    int eOutType = -1;
    const OGRField* min_fld = iter->GetMinValue(eOutType);
    ASSERT_NE(min_fld, nullptr);
    EXPECT_EQ(min_fld->Integer, 100);

    const OGRField* max_fld = iter->GetMaxValue(eOutType);
    ASSERT_NE(max_fld, nullptr);
    EXPECT_EQ(max_fld->Integer, 200);

    EXPECT_EQ(iter->GetNextRowSortedByValue(), 0);
    EXPECT_EQ(iter->GetNextRowSortedByValue(), 1);
    EXPECT_EQ(iter->GetNextRowSortedByValue(), -1);

    // Build V2 iterator with EQ
    OGRField eq_val;
    eq_val.Integer = 200;
    auto iter_eq = std::unique_ptr<FileGDBIterator>(
        FileGDBIterator::Build(&table, 1, TRUE, FGSO_EQ, OFTInteger, &eq_val));
    ASSERT_NE(iter_eq, nullptr);
    EXPECT_EQ(iter_eq->GetNextRowSortedByValue(), 1);
    EXPECT_EQ(iter_eq->GetNextRowSortedByValue(), -1);
  }

  // 2. Construct synthetic V2 .spx file
  {
    const std::string spx_path = std::string(kTestDir) + "/v2_test.spx";
    VSILFILE* fp = VSIFOpenL(spx_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);

    // Page 1: root non-leaf page
    std::vector<GByte> p1(65536, 0);
    uint32_t nSubPages = 1;
    memcpy(p1.data() + 8, &nSubPages, 4);
    uint64_t child = 2;
    memcpy(p1.data() + 12, &child, 8);
    memcpy(p1.data() + 20, &child, 8);
    // Value size 8, max per pages = (65536 - 20) / (8 + 8) = 4094
    // offset first val = 20 + 4094 * 8 = 32772
    int offset_val_sp1 = 20 + ((65536 - 20) / (8 + 8)) * 8;
    int64_t max_val = 0x7FFFFFFFFFFFFFFFLL;
    memcpy(p1.data() + offset_val_sp1, &max_val, 8);
    VSIFWriteL(p1.data(), 1, 65536, fp);

    // Page 2: leaf page
    std::vector<GByte> p2(65536, 0);
    uint32_t nFeatures = 2;
    memcpy(p2.data() + 8, &nFeatures, 4);
    uint64_t fid1 = 1, fid2 = 2;
    memcpy(p2.data() + 20, &fid1, 8);
    memcpy(p2.data() + 28, &fid2, 8);
    uint64_t sc1 = 10ULL + (1ULL << 29);
    uint64_t sc2 = 20ULL + (1ULL << 29);
    int64_t v1 = static_cast<int64_t>((sc1 << 31) | sc1);
    int64_t v2 = static_cast<int64_t>((sc2 << 31) | sc2);
    memcpy(p2.data() + offset_val_sp1, &v1, 8);
    memcpy(p2.data() + offset_val_sp1 + 8, &v2, 8);
    VSIFWriteL(p2.data(), 1, 65536, fp);

    // Trailer (30 bytes)
    std::vector<GByte> tr(30, 0);
    tr[0] = 8;
    uint32_t mg = 1, dp = 2, vr = 2;
    uint64_t vc = 2;
    memcpy(tr.data() + 2, &mg, 4);
    memcpy(tr.data() + 6, &dp, 4);
    memcpy(tr.data() + 10, &vc, 8);
    memcpy(tr.data() + 26, &vr, 4);
    VSIFWriteL(tr.data(), 1, 30, fp);
    VSIFCloseL(fp);

    OGREnvelope env;
    env.MinX = 50.0;
    env.MaxX = 250.0;
    env.MinY = 50.0;
    env.MaxY = 250.0;
    auto sp_iter = std::unique_ptr<FileGDBSpatialIndexIterator>(
        FileGDBSpatialIndexIterator::Build(&table, env));
    ASSERT_NE(sp_iter, nullptr);
    EXPECT_GE(sp_iter->GetNextRowSortedByFID(), 0);
  }
}

}  // namespace
}  // namespace autotest2
