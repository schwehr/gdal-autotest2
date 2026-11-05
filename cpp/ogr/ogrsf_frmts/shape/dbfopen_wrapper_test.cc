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

#include <cstdint>
#include <cstring>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

static decltype(SAHooks::FOpen) g_orig_fopen = nullptr;
static decltype(SAHooks::FSeek) g_orig_fseek = nullptr;
static decltype(SAHooks::FRead) g_orig_fread = nullptr;
static decltype(SAHooks::FWrite) g_orig_fwrite = nullptr;
static bool g_fail_fopen = false;
static bool g_fail_fseek = false;
static bool g_fail_fread = false;
static bool g_fail_fwrite = false;

SAFile HookFOpen(const char* path, const char* mode, void* user_data) {
  if (g_fail_fopen) return nullptr;
  return g_orig_fopen(path, mode, user_data);
}

SAOffset HookFSeek(SAFile file, SAOffset offset, int whence) {
  if (g_fail_fseek) return -1;
  return g_orig_fseek(file, offset, whence);
}

SAOffset HookFRead(void* buf, SAOffset size, SAOffset nmemb, SAFile file) {
  if (g_fail_fread) return 0;
  return g_orig_fread(buf, size, nmemb, file);
}

SAOffset HookFWrite(const void* buf, SAOffset size, SAOffset nmemb,
                    SAFile file) {
  if (g_fail_fwrite) return 0;
  return g_orig_fwrite(buf, size, nmemb, file);
}

TEST(DbfOpenTest, CreateAddFieldsAndWriteReadAttributes) {
  constexpr const char* kDbfPath = "/vsimem/test_crud.dbf";
  constexpr const char* kCpgPath = "/vsimem/test_crud.cpg";
  auto cleanup = absl::MakeCleanup([kDbfPath, kCpgPath]() {
    VSIUnlink(kDbfPath);
    VSIUnlink(kCpgPath);
  });

  // Create DBF with UTF-8 codepage creating .cpg sidecar
  DBFHandle hDBF = DBFCreateEx(kDbfPath, "UTF-8");
  ASSERT_THAT(hDBF, NotNull());

  EXPECT_EQ(DBFGetFieldCount(hDBF), 0);
  EXPECT_EQ(DBFGetRecordCount(hDBF), 0);

  // Add various field types
  EXPECT_EQ(DBFAddField(hDBF, "STR_FLD", FTString, 20, 0), 0);
  EXPECT_EQ(DBFAddField(hDBF, "INT_FLD", FTInteger, 9, 0), 1);
  EXPECT_EQ(DBFAddField(hDBF, "DBL_FLD", FTDouble, 12, 4), 2);
  EXPECT_EQ(DBFAddField(hDBF, "DATE_FLD", FTDate, 8, 0), 3);
  EXPECT_EQ(DBFAddField(hDBF, "LOG_FLD", FTLogical, 1, 0), 4);

  // Add native types
  EXPECT_EQ(DBFAddNativeFieldType(hDBF, "NAT_NUM", 'N', 8, 2), 5);
  EXPECT_EQ(DBFAddNativeFieldType(hDBF, "NAT_CHAR", 'C', 15, 0), 6);

  // Test field metadata APIs
  EXPECT_EQ(DBFGetFieldCount(hDBF), 7);
  EXPECT_EQ(DBFGetFieldIndex(hDBF, "STR_FLD"), 0);
  EXPECT_EQ(DBFGetFieldIndex(hDBF, "str_fld"), 0);  // case-insensitive
  EXPECT_EQ(DBFGetFieldIndex(hDBF, "NON_EXIST"), -1);

  char szName[12];
  int nWidth = 0;
  int nDecimals = 0;
  EXPECT_EQ(DBFGetFieldInfo(hDBF, 0, szName, &nWidth, &nDecimals), FTString);
  EXPECT_STREQ(szName, "STR_FLD");
  EXPECT_EQ(nWidth, 20);
  EXPECT_EQ(nDecimals, 0);

  EXPECT_EQ(DBFGetFieldInfo(hDBF, 1, szName, &nWidth, &nDecimals), FTInteger);
  EXPECT_EQ(DBFGetFieldInfo(hDBF, 2, szName, &nWidth, &nDecimals), FTDouble);
  EXPECT_EQ(DBFGetFieldInfo(hDBF, 3, szName, &nWidth, &nDecimals), FTDate);
  EXPECT_EQ(DBFGetFieldInfo(hDBF, 4, szName, &nWidth, &nDecimals), FTLogical);
  EXPECT_EQ(DBFGetFieldInfo(hDBF, -1, szName, &nWidth, &nDecimals), FTInvalid);

  EXPECT_EQ(DBFGetNativeFieldType(hDBF, 0), 'C');
  EXPECT_EQ(DBFGetNativeFieldType(hDBF, 1), 'N');
  EXPECT_EQ(DBFGetNativeFieldType(hDBF, 2), 'N');
  EXPECT_EQ(DBFGetNativeFieldType(hDBF, 3), 'D');
  EXPECT_EQ(DBFGetNativeFieldType(hDBF, 4), 'L');
  EXPECT_EQ(DBFGetNativeFieldType(hDBF, 99), ' ');

  // Write record 0 with valid values
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 0, 0, "Hello World"));
  EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 1, 12345));
  EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 0, 2, 123.4567));
  SHPDate dateVal = {2026, 11, 3};
  EXPECT_TRUE(DBFWriteDateAttribute(hDBF, 0, 3, &dateVal));
  EXPECT_TRUE(DBFWriteLogicalAttribute(hDBF, 0, 4, 'T'));
  EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 0, 5, 42.5));
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 0, 6, "Native Char"));

  // Write record 1 with NULL values
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 0));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 1));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 2));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 3));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 4));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 5));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 6));

  // Write record 2 using DBFWriteAttributeDirectly
  EXPECT_TRUE(DBFWriteAttributeDirectly(hDBF, 2, 0, "Direct String"));
  EXPECT_TRUE(DBFWriteLogicalAttribute(hDBF, 2, 4, 'F'));

  // Write record 3
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 3, 0, "Record Three"));
  EXPECT_EQ(DBFGetRecordCount(hDBF), 4);

  // Test DBFWriteTuple / DBFReadTuple
  const char* tuple = DBFReadTuple(hDBF, 0);
  ASSERT_THAT(tuple, NotNull());
  EXPECT_TRUE(DBFWriteTuple(hDBF, 3, tuple));

  // Read back and verify record 0
  EXPECT_STREQ(DBFReadStringAttribute(hDBF, 0, 0), "Hello World");
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, 0, 1), 12345);
  EXPECT_NEAR(DBFReadDoubleAttribute(hDBF, 0, 2), 123.4567, 1e-4);
  SHPDate readDate = DBFReadDateAttribute(hDBF, 0, 3);
  EXPECT_EQ(readDate.year, 2026);
  EXPECT_EQ(readDate.month, 11);
  EXPECT_EQ(readDate.day, 3);
  EXPECT_STREQ(DBFReadLogicalAttribute(hDBF, 0, 4), "T");
  EXPECT_FALSE(DBFIsAttributeNULL(hDBF, 0, 0));
  EXPECT_FALSE(DBFIsAttributeNULL(hDBF, 0, 1));

  // Verify record 1 NULL attributes
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 0));
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 1));
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 2));
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 3));
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 4));

  // Test record deletion
  EXPECT_FALSE(DBFIsRecordDeleted(hDBF, 0));
  EXPECT_TRUE(DBFMarkRecordDeleted(hDBF, 0, TRUE));
  EXPECT_TRUE(DBFIsRecordDeleted(hDBF, 0));
  EXPECT_TRUE(DBFMarkRecordDeleted(hDBF, 0, FALSE));
  EXPECT_FALSE(DBFIsRecordDeleted(hDBF, 0));

  // Test DBFSetLastModifiedDate and DBFSetWriteEndOfFileChar
  DBFSetLastModifiedDate(hDBF, 126, 11, 3);
  DBFSetWriteEndOfFileChar(hDBF, TRUE);
  DBFUpdateHeader(hDBF);

  // Close and reopen in read-only mode
  DBFClose(hDBF);

  DBFHandle hReadOnly = DBFOpen(kDbfPath, "rb");
  ASSERT_THAT(hReadOnly, NotNull());
  EXPECT_EQ(DBFGetRecordCount(hReadOnly), 4);
  EXPECT_STREQ(DBFGetCodePage(hReadOnly), "UTF-8");
  EXPECT_STREQ(DBFReadStringAttribute(hReadOnly, 0, 0), "Hello World");
  DBFClose(hReadOnly);
}

TEST(DbfOpenTest, SchemaMutationsAfterRecordsWritten) {
  constexpr const char* kDbfPath = "/vsimem/test_mutate.dbf";
  auto cleanup = absl::MakeCleanup([kDbfPath]() { VSIUnlink(kDbfPath); });

  DBFHandle hDBF = DBFCreate(kDbfPath);  // default LDID/87
  ASSERT_THAT(hDBF, NotNull());
  EXPECT_STREQ(DBFGetCodePage(hDBF), "LDID/87");

  EXPECT_EQ(DBFAddField(hDBF, "FLD_A", FTString, 10, 0), 0);
  EXPECT_EQ(DBFAddField(hDBF, "FLD_B", FTInteger, 6, 0), 1);
  EXPECT_EQ(DBFAddField(hDBF, "FLD_C", FTDouble, 8, 2), 2);

  // Write 3 records
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(DBFWriteStringAttribute(hDBF, i, 0, "Record"));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, i, 1, (i + 1) * 10));
    EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, i, 2, (i + 1) * 1.5));
  }
  EXPECT_EQ(DBFGetRecordCount(hDBF), 3);

  // Test DBFAddNativeFieldType AFTER records written (causes record rewriting)
  EXPECT_EQ(DBFAddNativeFieldType(hDBF, "FLD_D", 'C', 10, 0), 3);
  EXPECT_EQ(DBFGetFieldCount(hDBF), 4);
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 0, 3));

  // Test DBFReorderFields
  int panMap[4] = {3, 2, 1, 0};  // reverse order
  EXPECT_TRUE(DBFReorderFields(hDBF, panMap));
  EXPECT_STREQ(DBFReadStringAttribute(hDBF, 0, 3), "Record");
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, 0, 2), 10);
  EXPECT_NEAR(DBFReadDoubleAttribute(hDBF, 0, 1), 1.5, 1e-3);

  // Test DBFAlterFieldDefn (alter field name and width)
  EXPECT_TRUE(DBFAlterFieldDefn(hDBF, 3, "RENAMED_A", 'C', 15, 0));
  char szName[12];
  int nWidth = 0;
  int nDec = 0;
  DBFGetFieldInfo(hDBF, 3, szName, &nWidth, &nDec);
  EXPECT_STREQ(szName, "RENAMED_A");
  EXPECT_EQ(nWidth, 15);
  EXPECT_STREQ(DBFReadStringAttribute(hDBF, 0, 3), "Record");

  // Test DBFDeleteField
  EXPECT_TRUE(DBFDeleteField(hDBF, 0));  // delete FLD_D
  EXPECT_EQ(DBFGetFieldCount(hDBF), 3);
  EXPECT_NEAR(DBFReadDoubleAttribute(hDBF, 0, 0), 1.5, 1e-3);

  // Test DBFCloneEmpty
  constexpr const char* kClonePath = "/vsimem/test_clone.dbf";
  auto cleanupClone =
      absl::MakeCleanup([kClonePath]() { VSIUnlink(kClonePath); });
  DBFHandle hClone = DBFCloneEmpty(hDBF, kClonePath);
  ASSERT_THAT(hClone, NotNull());
  EXPECT_EQ(DBFGetFieldCount(hClone), 3);
  EXPECT_EQ(DBFGetRecordCount(hClone), 0);
  DBFClose(hClone);

  DBFClose(hDBF);
}

TEST(DbfOpenTest, DataFormattingAndNullHandling) {
  constexpr const char* kDbfPath = "/vsimem/test_types.dbf";
  auto cleanup = absl::MakeCleanup([kDbfPath]() { VSIUnlink(kDbfPath); });

  DBFHandle hDBF = DBFCreate(kDbfPath);
  ASSERT_THAT(hDBF, NotNull());

  EXPECT_EQ(DBFAddField(hDBF, "STR_VAL", FTString, 25, 0), 0);
  EXPECT_EQ(DBFAddField(hDBF, "INT_VAL", FTInteger, 9, 0), 1);
  EXPECT_EQ(DBFAddField(hDBF, "DBL_VAL", FTDouble, 12, 4), 2);
  EXPECT_EQ(DBFAddField(hDBF, "DATE_VAL", FTDate, 8, 0), 3);
  EXPECT_EQ(DBFAddField(hDBF, "LOG_VAL", FTLogical, 1, 0), 4);

  // Write record 0 with various logical and number representations
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 0, 0, "  Spaces Trimmed  "));
  EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 1, -9876));
  EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 0, 2, -123.45));
  SHPDate date0 = {1999, 12, 31};
  EXPECT_TRUE(DBFWriteDateAttribute(hDBF, 0, 3, &date0));
  EXPECT_TRUE(DBFWriteLogicalAttribute(hDBF, 0, 4, 'T'));

  // Write record 1: logical 'F', date null
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 1, 0, ""));
  EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 1, 1, 0));
  EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 1, 2, 0.0));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 3));
  EXPECT_TRUE(DBFWriteLogicalAttribute(hDBF, 1, 4, 'F'));

  // Write record 2: logical NULL
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 2, 4));

  // Read back and verify
  EXPECT_STREQ(DBFReadStringAttribute(hDBF, 0, 0), "Spaces Trimmed");
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, 0, 1), -9876);
  EXPECT_NEAR(DBFReadDoubleAttribute(hDBF, 0, 2), -123.45, 1e-3);
  SHPDate d0 = DBFReadDateAttribute(hDBF, 0, 3);
  EXPECT_EQ(d0.year, 1999);
  EXPECT_EQ(d0.month, 12);
  EXPECT_EQ(d0.day, 31);
  EXPECT_STREQ(DBFReadLogicalAttribute(hDBF, 0, 4), "T");

  EXPECT_STREQ(DBFReadLogicalAttribute(hDBF, 1, 4), "F");
  EXPECT_STREQ(DBFReadLogicalAttribute(hDBF, 2, 4), "?");

  // Out of bounds reads return 0 / null / empty
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, -1, 0), 0);
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, 0, -1), 0);
  EXPECT_EQ(DBFReadDoubleAttribute(hDBF, 99, 0), 0.0);
  EXPECT_EQ(DBFReadStringAttribute(hDBF, 99, 0), nullptr);
  EXPECT_EQ(DBFReadLogicalAttribute(hDBF, 99, 0), nullptr);
  EXPECT_EQ(DBFReadDateAttribute(hDBF, 99, 0).year, 0);

  // Write double to 'D' field
  EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 0, 3, 20261103.0));

  // Write overflowing integer to small field (width 3)
  EXPECT_EQ(DBFAddNativeFieldType(hDBF, "SMALL_N", 'N', 3, 0), 5);
  EXPECT_FALSE(DBFWriteIntegerAttribute(hDBF, 0, 5, 123456));

  // Write invalid logical character
  EXPECT_FALSE(DBFWriteLogicalAttribute(hDBF, 0, 4, 'X'));

  // Write string exceeding field width on 'C' field
  EXPECT_EQ(DBFAddNativeFieldType(hDBF, "SMALL_C", 'C', 3, 0), 6);
  EXPECT_FALSE(DBFWriteStringAttribute(hDBF, 0, 6, "WAY_TOO_LONG"));

  // Write invalid dates
  SHPDate badDate1 = {-1, 5, 5};
  EXPECT_FALSE(DBFWriteDateAttribute(hDBF, 0, 3, &badDate1));
  SHPDate badDate2 = {2026, 100, 5};
  EXPECT_FALSE(DBFWriteDateAttribute(hDBF, 0, 3, &badDate2));
  SHPDate badDate3 = {2026, 5, 100};
  EXPECT_FALSE(DBFWriteDateAttribute(hDBF, 0, 3, &badDate3));
  EXPECT_FALSE(DBFWriteDateAttribute(hDBF, 0, 3, nullptr));

  // Write malformed date string directly and read back
  EXPECT_TRUE(DBFWriteAttributeDirectly(hDBF, 0, 3, "BAD_DATE"));
  SHPDate malformedDate = DBFReadDateAttribute(hDBF, 0, 3);
  EXPECT_EQ(malformedDate.year, 0);

  // DBFIsAttributeNULL on dates
  EXPECT_TRUE(DBFWriteAttributeDirectly(hDBF, 0, 3, "00000000"));
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 0, 3));
  EXPECT_TRUE(DBFWriteAttributeDirectly(hDBF, 0, 3, "00002026"));
  EXPECT_FALSE(DBFIsAttributeNULL(hDBF, 0, 3));

  // Test DBFMarkRecordDeleted edge cases
  EXPECT_FALSE(DBFMarkRecordDeleted(hDBF, -1, TRUE));
  EXPECT_TRUE(DBFMarkRecordDeleted(hDBF, 0, FALSE));  // already undeleted

  DBFClose(hDBF);
}

TEST(DbfOpenTest, AlterFieldShrinkExpandAndEmptyTable) {
  constexpr const char* kDbfPath = "/vsimem/alter_shrink_expand.dbf";
  auto cleanup = absl::MakeCleanup([kDbfPath]() { VSIUnlink(kDbfPath); });

  DBFHandle hDBF = DBFCreate(kDbfPath);
  ASSERT_THAT(hDBF, NotNull());

  EXPECT_EQ(DBFAddField(hDBF, "FLD_0", FTString, 10, 0), 0);
  EXPECT_EQ(DBFAddField(hDBF, "FLD_1", FTInteger, 6, 0), 1);
  EXPECT_EQ(DBFAddField(hDBF, "FLD_2", FTString, 10, 0), 2);

  // Write record 0 with values and record 1 with NULLs
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 0, 0, "Hello"));
  EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 1, 1234));
  EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 0, 2, "World"));

  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 0));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 1));
  EXPECT_TRUE(DBFWriteNULLAttribute(hDBF, 1, 2));

  // Expand middle field FLD_1 from 6 to 10
  EXPECT_TRUE(DBFAlterFieldDefn(hDBF, 1, "FLD_1_EXP", 'N', 10, 2));
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, 0, 1), 1234);
  EXPECT_STREQ(DBFReadStringAttribute(hDBF, 0, 2), "World");
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 1));

  // Shrink middle field FLD_1 from 10 to 5
  EXPECT_TRUE(DBFAlterFieldDefn(hDBF, 1, "FLD_1_SHR", 'N', 5, 0));
  EXPECT_EQ(DBFReadIntegerAttribute(hDBF, 0, 1), 1234);
  EXPECT_STREQ(DBFReadStringAttribute(hDBF, 0, 2), "World");
  EXPECT_TRUE(DBFIsAttributeNULL(hDBF, 1, 1));

  // Invalid AlterFieldDefn arguments
  EXPECT_FALSE(DBFAlterFieldDefn(hDBF, -1, "BAD", 'N', 5, 0));
  EXPECT_EQ(DBFAlterFieldDefn(hDBF, 1, "BAD", 'N', 0, 0), -1);
  EXPECT_TRUE(DBFAlterFieldDefn(hDBF, 1, "BAD", 'N', 300, 0));  // clamps to 255
  EXPECT_FALSE(DBFDeleteField(hDBF, -1));
  EXPECT_FALSE(DBFDeleteField(hDBF, 99));

  DBFClose(hDBF);

  // Alter, delete, and reorder on empty DBF before records are written
  constexpr const char* kEmptyTablePath = "/vsimem/empty_table.dbf";
  auto cleanupEmpty =
      absl::MakeCleanup([kEmptyTablePath]() { VSIUnlink(kEmptyTablePath); });
  DBFHandle hEmpty = DBFCreate(kEmptyTablePath);
  ASSERT_THAT(hEmpty, NotNull());
  EXPECT_TRUE(DBFReorderFields(hEmpty, nullptr));  // 0 fields
  EXPECT_EQ(DBFAddField(hEmpty, "FIELD_A", FTString, 10, 0), 0);
  EXPECT_EQ(DBFAddField(hEmpty, "FIELD_B", FTInteger, 6, 0), 1);
  EXPECT_TRUE(DBFAlterFieldDefn(hEmpty, 0, "RENAMED_A", 'C', 12, 0));
  EXPECT_TRUE(DBFDeleteField(hEmpty, 1));
  EXPECT_EQ(DBFGetFieldCount(hEmpty), 1);
  DBFClose(hEmpty);
}

TEST(DbfOpenTest, FreshDbfDirectWriteAndTuple) {
  constexpr const char* kFreshPath = "/vsimem/fresh_write.dbf";
  auto cleanup = absl::MakeCleanup([kFreshPath]() { VSIUnlink(kFreshPath); });

  // DBFWriteTuple on freshly created DBF
  DBFHandle hFresh = DBFCreate(kFreshPath);
  ASSERT_THAT(hFresh, NotNull());
  EXPECT_EQ(DBFAddField(hFresh, "FLD", FTString, 10, 0), 0);
  EXPECT_FALSE(DBFWriteTuple(hFresh, -1, " 1234567890"));
  EXPECT_TRUE(DBFWriteTuple(hFresh, 0, " 1234567890"));
  DBFClose(hFresh);

  // DBFWriteAttributeDirectly on freshly created DBF with long string
  VSIUnlink(kFreshPath);
  hFresh = DBFCreate(kFreshPath);
  ASSERT_THAT(hFresh, NotNull());
  EXPECT_EQ(DBFAddField(hFresh, "FLD", FTString, 5, 0), 0);
  EXPECT_FALSE(DBFWriteAttributeDirectly(hFresh, -1, 0, "X"));
  EXPECT_TRUE(DBFWriteAttributeDirectly(hFresh, 0, 0, "LONG_STRING_CLIPPED"));
  DBFClose(hFresh);
}

TEST(DbfOpenTest, AccessModesAndUppercaseFallback) {
  constexpr const char* kDbfPath = "/vsimem/access_modes.dbf";
  auto cleanup = absl::MakeCleanup([kDbfPath]() { VSIUnlink(kDbfPath); });
  DBFHandle h = DBFCreate(kDbfPath);
  ASSERT_THAT(h, NotNull());
  EXPECT_EQ(DBFAddField(h, "F1", FTString, 10, 0), 0);
  // Add field with width 200 to trigger realloc of pszWorkField in
  // DBFReadAttribute
  EXPECT_EQ(DBFAddField(h, "F2", FTString, 200, 0), 1);
  EXPECT_TRUE(DBFWriteStringAttribute(h, 0, 0, "Val1"));
  EXPECT_TRUE(DBFWriteStringAttribute(h, 0, 1, "Val2"));
  DBFClose(h);

  // Open with "r" and "r+"
  DBFHandle hR = DBFOpen(kDbfPath, "r");
  ASSERT_THAT(hR, NotNull());
  EXPECT_STREQ(DBFReadStringAttribute(hR, 0, 0), "Val1");
  EXPECT_STREQ(DBFReadStringAttribute(hR, 0, 1), "Val2");  // triggers realloc
  DBFClose(hR);

  DBFHandle hRPlus = DBFOpen(kDbfPath, "r+");
  ASSERT_THAT(hRPlus, NotNull());
  DBFClose(hRPlus);

  DBFHandle hRPlusB = DBFOpen(kDbfPath, "r+b");
  ASSERT_THAT(hRPlusB, NotNull());
  DBFClose(hRPlusB);

  // Invalid access mode returns nullptr
  EXPECT_EQ(DBFOpen(kDbfPath, "w"), nullptr);
  EXPECT_EQ(DBFOpen(kDbfPath, "invalid"), nullptr);

  // Uppercase .DBF and .CPG fallback
  constexpr const char* kUpperDbf = "/vsimem/UPPER_LAYER.DBF";
  constexpr const char* kUpperCpg = "/vsimem/UPPER_LAYER.CPG";
  vsi_l_offset nDataLen = 0;
  GByte* paby = VSIGetMemFileBuffer(kDbfPath, &nDataLen, FALSE);
  VSIFCloseL(VSIFileFromMemBuffer(kUpperDbf, paby, nDataLen, FALSE));
  unsigned char cpgContent[] = "UTF-8";
  VSIFCloseL(VSIFileFromMemBuffer(
      kUpperCpg, cpgContent, strlen(reinterpret_cast<const char*>(cpgContent)),
      FALSE));
  auto cleanupUpper = absl::MakeCleanup([kUpperDbf, kUpperCpg]() {
    VSIUnlink(kUpperDbf);
    VSIUnlink(kUpperCpg);
  });

  DBFHandle hUpper = DBFOpen("/vsimem/UPPER_LAYER.dbf", "rb");
  ASSERT_THAT(hUpper, NotNull());
  EXPECT_STREQ(DBFGetCodePage(hUpper), "UTF-8");
  DBFClose(hUpper);
}

TEST(DbfOpenTest, ErrorPathsAndCorruptHeaders) {
  WithQuietHandler quiet_handler;

  // Non-existent file
  EXPECT_EQ(DBFOpen("/vsimem/does_not_exist.dbf", "rb"), nullptr);

  // Truncated header < 32 bytes
  constexpr const char* kShortPath = "/vsimem/short.dbf";
  unsigned char shortHdr[16] = {3, 126, 11, 3, 0, 0, 0, 0};
  VSIFCloseL(
      VSIFileFromMemBuffer(kShortPath, shortHdr, sizeof(shortHdr), FALSE));
  auto cleanup1 = absl::MakeCleanup([kShortPath]() { VSIUnlink(kShortPath); });
  EXPECT_EQ(DBFOpen(kShortPath, "rb"), nullptr);

  // Header where field length exceeds record length
  constexpr const char* kBadLenPath = "/vsimem/bad_len.dbf";
  unsigned char badLenHdr[32 + 32 + 1];
  memset(badLenHdr, 0, sizeof(badLenHdr));
  badLenHdr[0] = 3;  // version
  uint16_t hdrLen = 65;
  uint16_t recLen = 5;  // record length = 5
  memcpy(badLenHdr + 8, &hdrLen, 2);
  memcpy(badLenHdr + 10, &recLen, 2);
  // Field 0 with width 20 > record length 5
  badLenHdr[32] = 'F';
  badLenHdr[32 + 11] = 'C';
  badLenHdr[32 + 16] = 20;  // width = 20
  badLenHdr[64] = 0x0D;     // header terminator
  VSIFCloseL(
      VSIFileFromMemBuffer(kBadLenPath, badLenHdr, sizeof(badLenHdr), FALSE));
  auto cleanup2 =
      absl::MakeCleanup([kBadLenPath]() { VSIUnlink(kBadLenPath); });
  EXPECT_EQ(DBFOpen(kBadLenPath, "rb"), nullptr);

  // Invalid field width in DBFAddField
  constexpr const char* kBadFieldPath = "/vsimem/bad_field.dbf";
  auto cleanup3 =
      absl::MakeCleanup([kBadFieldPath]() { VSIUnlink(kBadFieldPath); });
  DBFHandle hBad = DBFCreate(kBadFieldPath);
  ASSERT_THAT(hBad, NotNull());
  EXPECT_EQ(DBFAddField(hBad, "BAD_W", FTString, 0, 0), -1);
  EXPECT_EQ(DBFAddField(hBad, "BAD_NEG", FTString, -5, 0), -1);

  // Header and record length limit checks in DBFAddNativeFieldType
  EXPECT_EQ(DBFAddNativeFieldType(hBad, "F1", 'C', 10, 0), 0);
  int origHdr = hBad->nHeaderLength;
  hBad->nHeaderLength = 65520;
  EXPECT_EQ(DBFAddNativeFieldType(hBad, "LIMIT_H", 'C', 5, 0), -1);
  hBad->nHeaderLength = origHdr;

  int origRec = hBad->nRecordLength;
  hBad->nRecordLength = 65530;
  EXPECT_EQ(DBFAddNativeFieldType(hBad, "LIMIT_R", 'C', 10, 0), -1);
  hBad->nRecordLength = origRec;
  DBFClose(hBad);

  // Zero record length with .cpg sidecar
  constexpr const char* kZeroRecPath = "/vsimem/zero_rec.dbf";
  constexpr const char* kZeroCpgPath = "/vsimem/zero_rec.cpg";
  unsigned char zeroRecHdr[32] = {};
  zeroRecHdr[0] = 3;
  uint16_t zeroHdrLen = 32;
  uint16_t zeroRecLen = 0;
  memcpy(zeroRecHdr + 8, &zeroHdrLen, 2);
  memcpy(zeroRecHdr + 10, &zeroRecLen, 2);
  VSIFCloseL(VSIFileFromMemBuffer(kZeroRecPath, zeroRecHdr, sizeof(zeroRecHdr),
                                  FALSE));
  unsigned char cpgData[] = "UTF-8\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kZeroCpgPath, cpgData, strlen(reinterpret_cast<const char*>(cpgData)),
      FALSE));
  auto cleanupZeroRec = absl::MakeCleanup([kZeroRecPath, kZeroCpgPath]() {
    VSIUnlink(kZeroRecPath);
    VSIUnlink(kZeroCpgPath);
  });
  EXPECT_EQ(DBFOpen(kZeroRecPath, "rb"), nullptr);

  // Truncated field descriptor block with LDID
  constexpr const char* kTruncFieldPath = "/vsimem/trunc_field.dbf";
  unsigned char truncFieldHdr[32] = {};
  truncFieldHdr[0] = 3;
  uint16_t truncHdrLen = 64;  // claims 64 bytes header
  uint16_t truncRecLen = 10;
  memcpy(truncFieldHdr + 8, &truncHdrLen, 2);
  memcpy(truncFieldHdr + 10, &truncRecLen, 2);
  truncFieldHdr[29] = 87;  // LDID/87
  VSIFCloseL(VSIFileFromMemBuffer(kTruncFieldPath, truncFieldHdr,
                                  sizeof(truncFieldHdr), FALSE));
  auto cleanupTrunc =
      absl::MakeCleanup([kTruncFieldPath]() { VSIUnlink(kTruncFieldPath); });
  EXPECT_EQ(DBFOpen(kTruncFieldPath, "rb"), nullptr);

  // Early HEADER_RECORD_TERMINATOR in field descriptor loop
  constexpr const char* kEarlyTermPath = "/vsimem/early_term.dbf";
  unsigned char earlyTermHdr[32 + 32 + 32 + 1] = {};
  earlyTermHdr[0] = 3;
  uint16_t earlyHdrLen = 96;
  uint16_t earlyRecLen = 11;
  memcpy(earlyTermHdr + 8, &earlyHdrLen, 2);
  memcpy(earlyTermHdr + 10, &earlyRecLen, 2);
  memcpy(earlyTermHdr + 32, "FIELD1", 6);
  earlyTermHdr[32 + 11] = 'C';
  earlyTermHdr[32 + 16] = 10;
  earlyTermHdr[64] = 0x0D;  // terminator
  VSIFCloseL(VSIFileFromMemBuffer(kEarlyTermPath, earlyTermHdr,
                                  sizeof(earlyTermHdr), FALSE));
  auto cleanupEarly =
      absl::MakeCleanup([kEarlyTermPath]() { VSIUnlink(kEarlyTermPath); });
  DBFHandle hEarly = DBFOpen(kEarlyTermPath, "rb");
  ASSERT_THAT(hEarly, NotNull());
  EXPECT_EQ(DBFGetFieldCount(hEarly), 1);
  DBFClose(hEarly);

  // Close null handle is a safe no-op
  DBFClose(nullptr);
}

TEST(DbfOpenTest, SchemaMutationsShortReadHandling) {
  WithQuietHandler quiet_handler;

  auto createCorruptRecCountDbf = [](const char* path) {
    DBFHandle h = DBFCreate(path);
    DBFAddNativeFieldType(h, "F1", 'C', 5, 0);
    DBFAddNativeFieldType(h, "F2", 'N', 5, 0);
    DBFWriteStringAttribute(h, 0, 0, "A");
    DBFWriteIntegerAttribute(h, 0, 1, 1);
    DBFClose(h);
    DBFHandle hOpen = DBFOpen(path, "r+b");
    hOpen->nRecords = 5;  // Pretend 5 records when only 1 exists
    return hOpen;
  };

  // 1. DBFAddNativeFieldType short read when shifting records
  constexpr const char* kMutAdd = "/vsimem/mut_add.dbf";
  auto clMutAdd = absl::MakeCleanup([kMutAdd]() { VSIUnlink(kMutAdd); });
  DBFHandle h1 = createCorruptRecCountDbf(kMutAdd);
  ASSERT_THAT(h1, NotNull());
  EXPECT_EQ(DBFAddNativeFieldType(h1, "NEW_F", 'C', 5, 0), -1);
  h1->bUpdated = FALSE;
  h1->bNoHeader = FALSE;
  DBFClose(h1);

  // 2. DBFDeleteField short read
  constexpr const char* kMutDel = "/vsimem/mut_del.dbf";
  auto clMutDel = absl::MakeCleanup([kMutDel]() { VSIUnlink(kMutDel); });
  DBFHandle h2 = createCorruptRecCountDbf(kMutDel);
  ASSERT_THAT(h2, NotNull());
  EXPECT_FALSE(DBFDeleteField(h2, 0));
  h2->bUpdated = FALSE;
  h2->bNoHeader = FALSE;
  DBFClose(h2);

  // 3. DBFReorderFields short read
  constexpr const char* kMutReorder = "/vsimem/mut_reorder.dbf";
  auto clMutReorder =
      absl::MakeCleanup([kMutReorder]() { VSIUnlink(kMutReorder); });
  DBFHandle h3 = createCorruptRecCountDbf(kMutReorder);
  ASSERT_THAT(h3, NotNull());
  int map[2] = {1, 0};
  EXPECT_FALSE(DBFReorderFields(h3, map));
  h3->bUpdated = FALSE;
  h3->bNoHeader = FALSE;
  DBFClose(h3);

  // 4. DBFAlterFieldDefn shrink short read
  constexpr const char* kMutShrink = "/vsimem/mut_shrink.dbf";
  auto clMutShrink =
      absl::MakeCleanup([kMutShrink]() { VSIUnlink(kMutShrink); });
  DBFHandle h4 = createCorruptRecCountDbf(kMutShrink);
  ASSERT_THAT(h4, NotNull());
  EXPECT_FALSE(DBFAlterFieldDefn(h4, 0, "F1", 'C', 2, 0));
  h4->bUpdated = FALSE;
  h4->bNoHeader = FALSE;
  DBFClose(h4);

  // 5. DBFAlterFieldDefn expand short read
  constexpr const char* kMutExpand = "/vsimem/mut_expand.dbf";
  auto clMutExpand =
      absl::MakeCleanup([kMutExpand]() { VSIUnlink(kMutExpand); });
  DBFHandle h5 = createCorruptRecCountDbf(kMutExpand);
  ASSERT_THAT(h5, NotNull());
  EXPECT_FALSE(DBFAlterFieldDefn(h5, 0, "F1", 'C', 15, 0));
  h5->bUpdated = FALSE;
  h5->bNoHeader = FALSE;
  DBFClose(h5);
}

TEST(DbfOpenTest, CustomHooksAndIoErrors) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  SASetupDefaultHooks(&hooks);

  g_orig_fopen = hooks.FOpen;
  g_orig_fseek = hooks.FSeek;
  g_orig_fread = hooks.FRead;
  g_orig_fwrite = hooks.FWrite;

  hooks.FOpen = HookFOpen;
  hooks.FSeek = HookFSeek;
  hooks.FRead = HookFRead;
  hooks.FWrite = HookFWrite;

  // 1. FOpen failure on create
  g_fail_fopen = true;
  EXPECT_EQ(DBFCreateLL("/vsimem/fail_create.dbf", "LDID/87", &hooks), nullptr);
  g_fail_fopen = false;

  // Create a valid DBF with 2 records
  constexpr const char* kHookPath = "/vsimem/hook_test.dbf";
  auto clHook = absl::MakeCleanup([kHookPath]() { VSIUnlink(kHookPath); });
  DBFHandle h = DBFCreateLL(kHookPath, "LDID/87", &hooks);
  ASSERT_THAT(h, NotNull());
  EXPECT_EQ(DBFAddNativeFieldType(h, "VAL", 'N', 5, 0), 0);
  EXPECT_TRUE(DBFWriteIntegerAttribute(h, 0, 0, 10));
  EXPECT_TRUE(DBFWriteIntegerAttribute(h, 1, 0, 20));
  DBFClose(h);

  h = DBFOpenLL(kHookPath, "r+b", &hooks);
  ASSERT_THAT(h, NotNull());

  // 2. FSeek failure during DBFFlushRecord
  h->nCurrentRecord = 0;
  h->bCurrentRecordModified = TRUE;
  h->bRequireNextWriteSeek = TRUE;
  g_fail_fseek = true;
  EXPECT_FALSE(DBFWriteIntegerAttribute(h, 1, 0, 99));
  g_fail_fseek = false;

  // 3. FWrite failure during DBFFlushRecord
  h->nCurrentRecord = 0;
  h->bCurrentRecordModified = TRUE;
  g_fail_fwrite = true;
  EXPECT_FALSE(DBFWriteIntegerAttribute(h, 1, 0, 99));
  g_fail_fwrite = false;

  // 4. FSeek failure during DBFLoadRecord
  h->nCurrentRecord = 1;
  g_fail_fseek = true;
  EXPECT_EQ(DBFReadIntegerAttribute(h, 0, 0), 0);
  g_fail_fseek = false;

  // 5. FRead failure during DBFLoadRecord
  h->nCurrentRecord = 1;
  g_fail_fread = true;
  EXPECT_EQ(DBFReadIntegerAttribute(h, 0, 0), 0);
  g_fail_fread = false;

  h->bCurrentRecordModified = FALSE;
  h->bUpdated = FALSE;
  DBFClose(h);
}

}  // namespace
}  // namespace autotest2
