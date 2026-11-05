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
// Tests OpenFileGDB .freelist management (filegdbtable_freelist.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable_freelist.cpp

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/filegdbtable_freelist_test";

class FileGDBTableFreelistTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIMkdir(kTestDir, 0755); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::unique_ptr<OpenFileGDB::FileGDBTable> CreateTable(
    const std::string& table_path, int tablx_offset_size = 4) {
  auto table = std::make_unique<OpenFileGDB::FileGDBTable>();
  if (!table->Create(table_path.c_str(), tablx_offset_size,
                     OpenFileGDB::FGTGT_NONE, false, false)) {
    return nullptr;
  }
  auto oid_field = std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  if (!table->CreateField(std::move(oid_field))) return nullptr;

  auto str_field = std::make_unique<OpenFileGDB::FileGDBField>(
      "str", "", OpenFileGDB::FGFT_STRING, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  if (!table->CreateField(std::move(str_field))) return nullptr;

  if (!table->Sync()) return nullptr;
  return table;
}

bool InsertFeature(OpenFileGDB::FileGDBTable* table, std::string_view val) {
  std::vector<OGRField> fields(table->GetFieldCount());
  for (auto& f : fields) {
    f.Set.nMarker1 = OGRUnsetMarker;
    f.Set.nMarker2 = OGRUnsetMarker;
    f.Set.nMarker3 = OGRUnsetMarker;
  }
  std::string str_val(val);
  fields[1].String = str_val.data();
  return table->CreateFeature(fields, nullptr);
}

TEST_F(FileGDBTableFreelistTest, BasicAddReuseAndEmptyFreeList) {
  const std::string table_path = std::string(kTestDir) + "/basic.gdbtable";
  auto table = CreateTable(table_path);
  ASSERT_NE(nullptr, table);

  // 1. Initial table has no freelist yet. CheckFreeListConsistency returns
  // true.
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // 2. Insert features with varying string lengths.
  // Feature 1: 50 chars (hole slot [40, 64))
  ASSERT_TRUE(InsertFeature(table.get(), std::string(50, 'A')));
  // Feature 2: 10 chars (hole slot [8, 16))
  ASSERT_TRUE(InsertFeature(table.get(), std::string(10, 'B')));
  // Feature 3: 20 chars (hole slot [16, 24))
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'C')));
  // Feature 4: 20 chars (hole slot [16, 24))
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'D')));

  // 3. Delete feature 2 (fid 2) -> creates .freelist with 1 page in slot [8,
  // 16).
  EXPECT_TRUE(table->DeleteFeature(2));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // Delete feature 3 (fid 3) and feature 4 (fid 4) -> slot [16, 24) now has 2
  // entries.
  EXPECT_TRUE(table->DeleteFeature(3));
  EXPECT_TRUE(table->DeleteFeature(4));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // Delete feature 1 (fid 1) -> slot [40, 64) has 1 entry.
  EXPECT_TRUE(table->DeleteFeature(1));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // 4. Reuse hole in slot [16, 24) by inserting a 20-char feature.
  // This removes an entry from slot [16, 24) while 1 entry remains in the page.
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'E')));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // Reuse the remaining hole in slot [16, 24).
  // This makes page 0 of slot [16, 24) empty, freeing the page.
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'F')));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // Reuse hole in slot [40, 64). Frees page of slot [40, 64).
  ASSERT_TRUE(InsertFeature(table.get(), std::string(50, 'G')));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // Reuse hole in slot [8, 16). Frees page of slot [8, 16).
  // Now all active slots have page count 0, marking freelist can be deleted.
  ASSERT_TRUE(InsertFeature(table.get(), std::string(10, 'H')));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // 5. Calling DeleteFreeList() unlinks .freelist file.
  const std::string freelist_path = std::string(kTestDir) + "/basic.freelist";
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(freelist_path.c_str(), &sStat));

  table->DeleteFreeList();
  EXPECT_NE(0, VSIStatL(freelist_path.c_str(), &sStat));
}

TEST_F(FileGDBTableFreelistTest, MultiPageFreelistAndReferencingPage) {
  const std::string table_path = std::string(kTestDir) + "/multipage.gdbtable";
  // 4-byte tablx offset -> entry size = 8 bytes.
  // Page size = 4096, header = 8 -> max entries per page = (4096 - 8) / 8 =
  // 511.
  auto table = CreateTable(table_path, /*tablx_offset_size=*/4);
  ASSERT_NE(nullptr, table);

  // 1. Insert 1 feature with 9 chars (hole size 15 in [8, 16)), then 515
  // features with 8 chars (hole size 14 in [8, 16)).
  ASSERT_TRUE(InsertFeature(table.get(), std::string(9, 'X')));
  for (int i = 0; i < 515; ++i) {
    ASSERT_TRUE(InsertFeature(table.get(), std::string(8, 'Y')));
  }

  // 2. Delete all 516 features.
  // The first feature deleted (fid 1, 9 chars) will be on page 0.
  // The next 510 deleted features fill page 0 (total 511 entries).
  // The remaining 5 deleted features overflow page 0 and allocate page 1
  // (linked to page 0).
  for (int i = 1; i <= 516; ++i) {
    ASSERT_TRUE(table->DeleteFeature(i));
  }
  EXPECT_TRUE(table->CheckFreeListConsistency());

  // Manually set page 0 entries count to 1 so that removing its single entry
  // empties page 0 while page 1 references page 0 (nReferencingPage !=
  // MINUS_ONE, covering lines 465-470).
  const std::string multipage_fl =
      std::string(kTestDir) + "/multipage.freelist";
  auto* fp_mp = VSIFOpenL(multipage_fl.c_str(), "rb+");
  ASSERT_NE(nullptr, fp_mp);
  uint32_t one = 1;
  ASSERT_EQ(1, VSIFWriteL(&one, sizeof(one), 1, fp_mp));
  VSIFCloseL(fp_mp);

  // 3. Insert a feature requiring the exact size of feature 1 (9 chars).
  // The search starts at page 1 (5 entries of 8 chars), does not find a match,
  // follows nPrevPage to page 0, and finds the 9-char entry.
  // Page 0 becomes empty and links referencing page 1 to nPrevPage (lines
  // 465-470).
  ASSERT_TRUE(InsertFeature(table.get(), std::string(9, 'Z')));

  // 4. Insert 5 features of 8 chars. This will exhaust all entries on page 1.
  // Page 1 becomes empty and is linked to the free pages list.
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(InsertFeature(table.get(), std::string(8, 'W')));
  }
  EXPECT_TRUE(table->CheckFreeListConsistency());
}

TEST_F(FileGDBTableFreelistTest, ConsistencyErrorsAndCorruptFreelist) {
  const std::string table_path = std::string(kTestDir) + "/corrupt.gdbtable";
  auto table = CreateTable(table_path);
  ASSERT_NE(nullptr, table);

  // Create one feature and delete it so that .freelist is properly created.
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'A')));
  ASSERT_TRUE(table->DeleteFeature(1));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  const std::string freelist_path = std::string(kTestDir) + "/corrupt.freelist";

  // Read original valid freelist bytes
  auto* fp = VSIFOpenL(freelist_path.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  VSIFSeekL(fp, 0, SEEK_END);
  const size_t file_size = static_cast<size_t>(VSIFTellL(fp));
  std::vector<GByte> valid_bytes(file_size);
  VSIFSeekL(fp, 0, SEEK_SET);
  ASSERT_EQ(1, VSIFReadL(valid_bytes.data(), file_size, 1, fp));
  VSIFCloseL(fp);

  auto overwrite_freelist = [&](const std::vector<GByte>& bytes) {
    auto* f = VSIFOpenL(freelist_path.c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(1, VSIFWriteL(bytes.data(), bytes.size(), 1, f));
    ASSERT_EQ(0, VSIFCloseL(f));
  };

  // 1. Bad file size (% 4096 != 344)
  {
    std::vector<GByte> bad = valid_bytes;
    bad.resize(bad.size() + 10);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Bad file size"));
  }

  // 2. First uint32 of trailer is not 1
  {
    std::vector<GByte> bad = valid_bytes;
    const size_t trailer_offset = bad.size() - 344;
    OpenFileGDB::WriteUInt32(bad, 2, trailer_offset);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unexpected value for first uint32"));
  }

  // 3. Cyclic link in free pages
  {
    std::vector<GByte> bad = valid_bytes;
    const size_t trailer_offset = bad.size() - 344;
    // Set last free page index in trailer to 0
    OpenFileGDB::WriteUInt32(bad, 0, trailer_offset + 4);
    // On page 0: header entries = 0, next free page = 0 (cycle)
    OpenFileGDB::WriteUInt32(bad, 0, 0);
    OpenFileGDB::WriteUInt32(bad, 0, 4);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cyclic page refererencing in free pages"));
  }

  // 4. Free page cannot be read (out of range page index)
  {
    std::vector<GByte> bad = valid_bytes;
    const size_t trailer_offset = bad.size() - 344;
    OpenFileGDB::WriteUInt32(bad, 999, trailer_offset + 4);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Can't read freelist page"));
  }

  // 5. Free page has unexpected non-zero entries count
  {
    std::vector<GByte> bad = valid_bytes;
    const size_t trailer_offset = bad.size() - 344;
    OpenFileGDB::WriteUInt32(bad, 0, trailer_offset + 4);
    OpenFileGDB::WriteUInt32(bad, 5, 0);  // entries count != 0
    OpenFileGDB::WriteUInt32(bad, 0xFFFFFFFFU, 4);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unexpected value for first uint32 of free page"));
  }

  // 6. Active page has unexpected entries count (0 or > max)
  {
    std::vector<GByte> bad = valid_bytes;
    // Page 0 entries = 0
    OpenFileGDB::WriteUInt32(bad, 0, 0);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unexpected value for entries count of active page"));
  }

  // 7. Active page entry with unexpected size (outside slot range)
  {
    std::vector<GByte> bad = valid_bytes;
    // Page 0 entry 0 size is set to 99999
    OpenFileGDB::WriteUInt32(bad, 99999, 8);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("contains free area of unexpected size"));
  }

  // 8. Active page entry points to invalid table offset
  {
    std::vector<GByte> bad = valid_bytes;
    // Write invalid offset 99999999
    OpenFileGDB::WriteUInt32(bad, 99999999, 8 + 4);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("invalid offset"));
  }

  // 9. Active page entry points to dead zone with wrong negated size
  {
    std::vector<GByte> bad = valid_bytes;
    // Read offset from entry 0
    const uint64_t offset = OpenFileGDB::GetUInt32(bad.data() + 8 + 4, 0);
    // In .gdbtable, overwrite that offset with a non-negated size (e.g. 10)
    auto* f_tbl = VSIFOpenL(table_path.c_str(), "rb+");
    ASSERT_NE(nullptr, f_tbl);
    VSIFSeekL(f_tbl, offset, SEEK_SET);
    uint32_t orig_val = 0;
    ASSERT_EQ(1, VSIFReadL(&orig_val, sizeof(orig_val), 1, f_tbl));
    VSIFSeekL(f_tbl, offset, SEEK_SET);
    uint32_t val = 10;
    ASSERT_EQ(1, VSIFWriteL(&val, sizeof(val), 1, f_tbl));
    VSIFCloseL(f_tbl);

    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("points to dead zone"));

    // Restore original value
    f_tbl = VSIFOpenL(table_path.c_str(), "rb+");
    ASSERT_NE(nullptr, f_tbl);
    VSIFSeekL(f_tbl, offset, SEEK_SET);
    ASSERT_EQ(1, VSIFWriteL(&orig_val, sizeof(orig_val), 1, f_tbl));
    VSIFCloseL(f_tbl);
  }

  // 10. Mismatched slot page count in trailer
  {
    std::vector<GByte> bad = valid_bytes;
    const size_t trailer_offset = bad.size() - 344;
    // For slot 2 (hole size [16, 24)), page count is at trailer_offset + 2 * 8
    // + 4. Set it to 99 instead of 1.
    OpenFileGDB::WriteUInt32(bad, 99, trailer_offset + 2 * 8 + 4);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unexpected value for page count of slot"));
  }

  // 11. Visited pages count mismatch (orphan page present in file)
  {
    std::vector<GByte> bad = valid_bytes;
    // Add an extra unreferenced page of 4096 bytes before the trailer
    const size_t trailer_offset = bad.size() - 344;
    std::vector<GByte> orphan(4096, 0);
    bad.insert(bad.begin() + trailer_offset, orphan.begin(), orphan.end());
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("pages have been visited, but there are"));
  }

  // 12. Cyclic page reference in active slot pages
  {
    std::vector<GByte> bad = valid_bytes;
    // Set page 0 next page (prev page in header) to 0
    OpenFileGDB::WriteUInt32(bad, 0, 4);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr(
            "Cyclic page refererencing or page referenced more than once"));
  }

  // 13. Out-of-bounds page index in slot
  {
    std::vector<GByte> bad = valid_bytes;
    const size_t trailer_offset = bad.size() - 344;
    // Slot 2 has hole size [16, 24). Point its head page to 999
    OpenFileGDB::WriteUInt32(bad, 999, trailer_offset + 2 * 8);
    overwrite_freelist(bad);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(table->CheckFreeListConsistency());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Can't read active page"));
  }
}

TEST_F(FileGDBTableFreelistTest, DuplicateOffsetInFreelist) {
  const std::string table_path = std::string(kTestDir) + "/dupoffset.gdbtable";
  auto table = CreateTable(table_path);
  ASSERT_NE(nullptr, table);

  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'A')));
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'B')));
  ASSERT_TRUE(table->DeleteFeature(1));
  ASSERT_TRUE(table->DeleteFeature(2));
  EXPECT_TRUE(table->CheckFreeListConsistency());

  const std::string freelist_path =
      std::string(kTestDir) + "/dupoffset.freelist";
  auto* fp = VSIFOpenL(freelist_path.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  VSIFSeekL(fp, 0, SEEK_END);
  const size_t file_size = static_cast<size_t>(VSIFTellL(fp));
  std::vector<GByte> valid_bytes(file_size);
  VSIFSeekL(fp, 0, SEEK_SET);
  ASSERT_EQ(1, VSIFReadL(valid_bytes.data(), file_size, 1, fp));
  VSIFCloseL(fp);

  // Copy entry 0 offset into entry 1 offset
  std::vector<GByte> bad = valid_bytes;
  memcpy(bad.data() + 8 + 8 + 4, bad.data() + 8 + 4, 4);
  auto* f = VSIFOpenL(freelist_path.c_str(), "wb");
  ASSERT_NE(nullptr, f);
  ASSERT_EQ(1, VSIFWriteL(bad.data(), bad.size(), 1, f));
  VSIFCloseL(f);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(table->CheckFreeListConsistency());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already referenced"));
}

TEST_F(FileGDBTableFreelistTest, SizeExtremesAndCandidateMemmove) {
  const std::string table_path = std::string(kTestDir) + "/extremes.gdbtable";
  auto table = CreateTable(table_path);
  ASSERT_NE(nullptr, table);

  // 1. Extreme large feature > 10MB
  ASSERT_TRUE(InsertFeature(table.get(), std::string(10000005, 'X')));
  EXPECT_TRUE(table->DeleteFeature(1));

  // 2. Extreme small feature < 8 bytes
  const std::string small_path = std::string(kTestDir) + "/small.gdbtable";
  auto small_table = std::make_unique<OpenFileGDB::FileGDBTable>();
  ASSERT_TRUE(small_table->Create(small_path.c_str(), 4,
                                  OpenFileGDB::FGTGT_NONE, false, false));
  auto oid_field = std::make_unique<OpenFileGDB::FileGDBField>(
      "ObjectID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(small_table->CreateField(std::move(oid_field)));
  ASSERT_TRUE(small_table->Sync());
  std::vector<OGRField> flds(1);
  flds[0].Set.nMarker1 = OGRUnsetMarker;
  flds[0].Set.nMarker2 = OGRUnsetMarker;
  flds[0].Set.nMarker3 = OGRUnsetMarker;
  ASSERT_TRUE(small_table->CreateFeature(flds, nullptr));
  EXPECT_TRUE(small_table->DeleteFeature(1));

  // 3. Candidate memmove (iBestCandidateEntry < nBestCandidateNumEntries - 1)
  const std::string memmove_path = std::string(kTestDir) + "/memmove.gdbtable";
  auto mtable = CreateTable(memmove_path);
  ASSERT_NE(nullptr, mtable);
  ASSERT_TRUE(InsertFeature(mtable.get(), std::string(20, 'A')));  // fid 1
  ASSERT_TRUE(InsertFeature(mtable.get(), std::string(22, 'B')));  // fid 2
  ASSERT_TRUE(mtable->DeleteFeature(1));  // entry 0 in slot [16, 24)
  ASSERT_TRUE(mtable->DeleteFeature(2));  // entry 1 in slot [16, 24)

  // Requesting 20 chars matches entry 0 exactly while entry 1 remains
  ASSERT_TRUE(InsertFeature(mtable.get(), std::string(20, 'C')));
  EXPECT_TRUE(mtable->CheckFreeListConsistency());
}

TEST_F(FileGDBTableFreelistTest, GetOffsetOfFreeAreaEdgeCases) {
  const std::string table_path = std::string(kTestDir) + "/edge.gdbtable";
  auto table = CreateTable(table_path);
  ASSERT_NE(nullptr, table);

  // 1. Initial empty table has no freelist; size < 8 returns OFFSET_MINUS_ONE.
  ASSERT_TRUE(InsertFeature(table.get(), std::string(20, 'A')));
  ASSERT_TRUE(table->DeleteFeature(1));

  const std::string freelist_path = std::string(kTestDir) + "/edge.freelist";

  // 2. Cyclic page referencing during candidate search
  auto* fp = VSIFOpenL(freelist_path.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  VSIFSeekL(fp, 0, SEEK_END);
  const size_t file_size = static_cast<size_t>(VSIFTellL(fp));
  std::vector<GByte> valid_bytes(file_size);
  VSIFSeekL(fp, 0, SEEK_SET);
  ASSERT_EQ(1, VSIFReadL(valid_bytes.data(), file_size, 1, fp));
  VSIFCloseL(fp);

  // Set previous page of page 0 to 0 (cycle)
  std::vector<GByte> cyclic = valid_bytes;
  OpenFileGDB::WriteUInt32(cyclic, 0, 4);
  auto* f = VSIFOpenL(freelist_path.c_str(), "wb");
  ASSERT_NE(nullptr, f);
  ASSERT_EQ(1, VSIFWriteL(cyclic.data(), cyclic.size(), 1, f));
  VSIFCloseL(f);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    // Inserting a feature of non-matching size so it walks prev pages
    EXPECT_TRUE(InsertFeature(table.get(), std::string(22, 'B')));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cyclic page refererencing"));
  }

  // 3. Candidate hole offset in table does not match negated size
  const std::string table_path2 =
      std::string(kTestDir) + "/corrupt_offset.gdbtable";
  auto table2 = CreateTable(table_path2);
  ASSERT_NE(nullptr, table2);
  ASSERT_TRUE(InsertFeature(table2.get(), std::string(20, 'A')));
  ASSERT_TRUE(table2->DeleteFeature(1));

  const std::string freelist_path2 =
      std::string(kTestDir) + "/corrupt_offset.freelist";
  auto* fp2 = VSIFOpenL(freelist_path2.c_str(), "rb");
  ASSERT_NE(nullptr, fp2);
  VSIFSeekL(fp2, 8 + 4, SEEK_SET);
  uint32_t hole_offset = 0;
  ASSERT_TRUE(OpenFileGDB::ReadUInt32(fp2, hole_offset));
  VSIFCloseL(fp2);

  // 3a. Overwrite with negated size that is too small (e.g. -4 un-negates to 4
  // < nSize - 4)
  auto* f_tbl = VSIFOpenL(table_path2.c_str(), "rb+");
  ASSERT_NE(nullptr, f_tbl);
  VSIFSeekL(f_tbl, hole_offset, SEEK_SET);
  uint32_t val_small = static_cast<uint32_t>(-4);
  ASSERT_EQ(1, VSIFWriteL(&val_small, sizeof(val_small), 1, f_tbl));
  VSIFCloseL(f_tbl);
  EXPECT_TRUE(InsertFeature(table2.get(), std::string(20, 'C')));

  // 3b. Overwrite hole_offset in .gdbtable with non-negated size
  f_tbl = VSIFOpenL(table_path2.c_str(), "rb+");
  ASSERT_NE(nullptr, f_tbl);
  VSIFSeekL(f_tbl, hole_offset, SEEK_SET);
  uint32_t val = 100;
  ASSERT_EQ(1, VSIFWriteL(&val, sizeof(val), 1, f_tbl));
  VSIFCloseL(f_tbl);
  EXPECT_TRUE(InsertFeature(table2.get(), std::string(20, 'D')));

  // 4. Slot with nPageIdx == MINUS_ONE (request hole slot [40, 64) with 50
  // chars)
  EXPECT_TRUE(InsertFeature(table2.get(), std::string(50, 'E')));

  // 5. Corrupt .freelist file size on insert and delete
  {
    auto* f_fl = VSIFOpenL(freelist_path2.c_str(), "ab");
    ASSERT_NE(nullptr, f_fl);
    std::vector<GByte> extra(10, 0);
    ASSERT_EQ(1, VSIFWriteL(extra.data(), extra.size(), 1, f_fl));
    VSIFCloseL(f_fl);
    EXPECT_TRUE(InsertFeature(table2.get(), std::string(20, 'F')));
    EXPECT_TRUE(table2->DeleteFeature(2));
  }

  // 6. Unexpected entry size + nPrevPage == MINUS_ONE + unreadable nPrevPage
  const std::string table_path3 = std::string(kTestDir) + "/edge3.gdbtable";
  auto table3 = CreateTable(table_path3);
  ASSERT_NE(nullptr, table3);
  ASSERT_TRUE(InsertFeature(table3.get(), std::string(20, 'A')));
  ASSERT_TRUE(table3->DeleteFeature(1));
  const std::string freelist_path3 = std::string(kTestDir) + "/edge3.freelist";

  // 6a. Unexpected entry size 99999 + nPrevPage == MINUS_ONE (lines 341-345,
  // 374, 408-409)
  {
    auto* f_fl = VSIFOpenL(freelist_path3.c_str(), "rb+");
    ASSERT_NE(nullptr, f_fl);
    VSIFSeekL(f_fl, 8, SEEK_SET);
    uint32_t bad_size = 99999;
    ASSERT_EQ(1, VSIFWriteL(&bad_size, sizeof(bad_size), 1, f_fl));
    VSIFCloseL(f_fl);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(InsertFeature(table3.get(), std::string(20, 'B')));
  }

  // 6b. Unexpected entry size 99999 + nPrevPage == 999 (unreadable prev page,
  // lines 392-394)
  {
    auto* f_fl = VSIFOpenL(freelist_path3.c_str(), "rb+");
    ASSERT_NE(nullptr, f_fl);
    VSIFSeekL(f_fl, 4, SEEK_SET);
    uint32_t bad_prev = 999;
    ASSERT_EQ(1, VSIFWriteL(&bad_prev, sizeof(bad_prev), 1, f_fl));
    VSIFCloseL(f_fl);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(InsertFeature(table3.get(), std::string(20, 'C')));
  }

  // 6c. Unreadable initial nPageIdx in slot trailer (lines 306-308)
  {
    auto* f_fl = VSIFOpenL(freelist_path3.c_str(), "rb+");
    ASSERT_NE(nullptr, f_fl);
    VSIFSeekL(f_fl, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(VSIFTellL(f_fl));
    VSIFSeekL(f_fl, sz - 344 + 2 * 8, SEEK_SET);
    uint32_t bad_page = 999;
    ASSERT_EQ(1, VSIFWriteL(&bad_page, sizeof(bad_page), 1, f_fl));
    VSIFCloseL(f_fl);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(InsertFeature(table3.get(), std::string(20, 'D')));
  }
}

}  // namespace
}  // namespace autotest2
