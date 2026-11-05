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
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "mitab.h"
#include "mitab_priv.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

namespace autotest2 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

TEST(TABDATFileTest, OpenCloseErrorsAndEdgeCases) {
  const std::string nonExistent = "/vsimem/dat_test_nonexistent.dat";
  TABDATFile datFile("");

  // Open non-existent file for read.
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.Open(nonExistent.c_str(), "r"));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.Open(nonExistent.c_str(), TABRead));
  CPLErrorReset();

  // Invalid access string mode.
  EXPECT_EQ(-1, datFile.Open(nonExistent.c_str(), "x"));
  CPLErrorReset();

  // Invalid combination: TABWrite or TABReadWrite with TABTableDBF.
  EXPECT_EQ(-1, datFile.Open(nonExistent.c_str(), TABWrite, TABTableDBF));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.Open(nonExistent.c_str(), TABReadWrite, TABTableDBF));
  CPLErrorReset();

  // Create valid file.
  const std::string validFile = "/vsimem/dat_test_valid.dat";
  absl::Cleanup cleanupValid = [validFile]() { VSIUnlink(validFile.c_str()); };
  EXPECT_EQ(0, datFile.Open(validFile.c_str(), "w"));

  // Double open without closing.
  EXPECT_EQ(-1, datFile.Open(validFile.c_str(), "w"));
  CPLErrorReset();

  EXPECT_EQ(0, datFile.Close());
  // Close already closed file.
  EXPECT_EQ(0, datFile.Close());

  // Closed file operations.
  EXPECT_EQ(-1, datFile.GetNumFields());
  EXPECT_EQ(-1, datFile.GetNumRecords());
  EXPECT_EQ(nullptr, datFile.GetRecordBlock(1));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.CommitRecordToFile());
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.MarkAsDeleted());
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.MarkRecordAsExisting());
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.AddField("col", TABFChar, 10));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.DeleteField(0));
  CPLErrorReset();
  int perm[1] = {0};
  EXPECT_EQ(-1, datFile.ReorderFields(perm));
  CPLErrorReset();
  OGRFieldDefn srcDefn("src", OFTString);
  OGRFieldDefn newDefn("new", OFTString);
  EXPECT_EQ(-1, datFile.AlterFieldDefn(0, &srcDefn, &newDefn, ALTER_ALL_FLAG));
  CPLErrorReset();
  EXPECT_EQ(0, datFile.SyncToDisk());
  EXPECT_EQ(TABFUnknown, datFile.GetFieldType(0));
  EXPECT_EQ(0, datFile.GetFieldWidth(0));
  EXPECT_EQ(0, datFile.GetFieldPrecision(0));
  EXPECT_EQ(-1, datFile.ValidateFieldInfoFromTAB(0, "col", TABFChar, 10, 0));
  CPLErrorReset();

  // Test field reads and writes on unopened / null m_poRecordBlock.
  EXPECT_STREQ("", datFile.ReadCharField(10));
  EXPECT_EQ(0, datFile.ReadIntegerField(4));
  EXPECT_EQ(0, datFile.ReadSmallIntField(2));
  EXPECT_EQ(0, datFile.ReadLargeIntField(8));
  EXPECT_EQ(0.0, datFile.ReadFloatField(8));
  EXPECT_EQ(0.0, datFile.ReadDecimalField(10));
  EXPECT_FALSE(datFile.ReadLogicalField(1));
  EXPECT_STREQ("", datFile.ReadDateField(8));
  int dummy = 0;
  EXPECT_EQ(-1, datFile.ReadDateField(8, &dummy, &dummy, &dummy));
  EXPECT_STREQ("", datFile.ReadTimeField(9));
  EXPECT_EQ(-1, datFile.ReadTimeField(9, &dummy, &dummy, &dummy, &dummy));
  EXPECT_STREQ("", datFile.ReadDateTimeField(17));
  EXPECT_EQ(-1, datFile.ReadDateTimeField(17, &dummy, &dummy, &dummy, &dummy,
                                          &dummy, &dummy, &dummy));
  EXPECT_EQ(-1, datFile.WriteCharField("a", 1, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteIntegerField(1, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteSmallIntField(1, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteLargeIntField(1, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteFloatField(1.0, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteLogicalField(true, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteDateField("2025/01/01", nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteDateField(2025, 1, 1, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteTimeField("12:00:00", nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteTimeField(1, 1, 1, 1, nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteDateTimeField("2025/01/01 12:00:00", nullptr, 0));
  EXPECT_EQ(-1, datFile.WriteDateTimeField(2025, 1, 1, 1, 1, 1, 1, nullptr, 0));
  CPLErrorReset();

  // Encoding getter and setter.
  datFile.SetEncoding("UTF-8");
  EXPECT_THAT(datFile.GetEncoding().c_str(), StrEq("UTF-8"));

  // Corrupt header file (less than 32 bytes or invalid record size / first
  // record ptr).
  const std::string corruptFile = "/vsimem/dat_test_corrupt.dat";
  absl::Cleanup cleanupCorrupt = [corruptFile]() {
    VSIUnlink(corruptFile.c_str());
  };
  GByte badHeader[32] = {};
  badHeader[0] = 0x03;
  // first record ptr < 32
  badHeader[8] = 16;
  badHeader[9] = 0;
  // record size 0
  badHeader[10] = 0;
  badHeader[11] = 0;
  VSILFILE* fp = VSIFOpenL(corruptFile.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  VSIFWriteL(badHeader, 1, sizeof(badHeader), fp);
  VSIFCloseL(fp);

  EXPECT_EQ(-1, datFile.Open(corruptFile.c_str(), "r"));
  CPLErrorReset();

  // Test early 0x0D terminator in field descriptors (lines 208-212).
  const std::string earlyTermFile = "/vsimem/dat_test_early_term.dat";
  absl::Cleanup cleanupEarlyTerm = [earlyTermFile]() {
    VSIUnlink(earlyTermFile.c_str());
  };
  GByte termHeader[96] = {};
  termHeader[0] = 0x03;
  termHeader[4] = 0;      // 0 records
  termHeader[8] = 96;     // first record ptr = 96 -> expects 2 fields
  termHeader[10] = 10;    // record size 10
  termHeader[32] = 0x0D;  // first field descriptor immediately terminated
  VSILFILE* termFp = VSIFOpenL(earlyTermFile.c_str(), "wb");
  ASSERT_THAT(termFp, NotNull());
  VSIFWriteL(termHeader, 1, sizeof(termHeader), termFp);
  VSIFCloseL(termFp);
  EXPECT_EQ(0, datFile.Open(earlyTermFile.c_str(), "r"));
  EXPECT_EQ(0, datFile.GetNumFields());
  EXPECT_EQ(0, datFile.Close());

  // Test operations in TABRead mode (not supported).
  EXPECT_EQ(0, datFile.Open(earlyTermFile.c_str(), TABRead));
  EXPECT_EQ(-1, datFile.SyncToDisk());
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.AddField("fld", TABFChar, 10));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.DeleteField(0));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.ReorderFields(perm));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.AlterFieldDefn(0, &srcDefn, &newDefn, ALTER_ALL_FLAG));
  CPLErrorReset();
  EXPECT_EQ(0, datFile.Close());
}

TEST(TABDATFileTest, NativeTableWriteAndReadAllTypes) {
  const std::string datPath = "/vsimem/dat_all_types.dat";
  const std::string indPath = "/vsimem/dat_all_types.ind";
  absl::Cleanup cleanupFiles = [datPath, indPath]() {
    VSIUnlink(datPath.c_str());
    VSIUnlink(indPath.c_str());
  };

  // 1. Create IND file to test index updating on Write*Field.
  TABINDFile indFile;
  ASSERT_EQ(0, indFile.Open(indPath.c_str(), "w"));
  EXPECT_EQ(1, indFile.CreateIndex(TABFChar, 20));
  EXPECT_EQ(2, indFile.CreateIndex(TABFInteger, 4));
  EXPECT_EQ(3, indFile.CreateIndex(TABFSmallInt, 2));
  EXPECT_EQ(4, indFile.CreateIndex(TABFLargeInt, 8));
  EXPECT_EQ(5, indFile.CreateIndex(TABFFloat, 8));
  EXPECT_EQ(6, indFile.CreateIndex(TABFDecimal, 12));
  EXPECT_EQ(7, indFile.CreateIndex(TABFLogical, 1));
  EXPECT_EQ(8, indFile.CreateIndex(TABFDate, 4));
  EXPECT_EQ(9, indFile.CreateIndex(TABFTime, 4));

  // 2. Open DAT file in write mode.
  TABDATFile datFile("UTF-8");
  ASSERT_EQ(0, datFile.Open(datPath.c_str(), TABWrite));

  // Add all 10 field types.
  EXPECT_EQ(0, datFile.AddField("char_fld", TABFChar, 20));
  EXPECT_EQ(0, datFile.AddField("int_fld", TABFInteger, 10));
  EXPECT_EQ(0, datFile.AddField("sint_fld", TABFSmallInt, 5));
  EXPECT_EQ(0, datFile.AddField("lint_fld", TABFLargeInt, 20));
  EXPECT_EQ(0, datFile.AddField("flt_fld", TABFFloat, 19, 8));
  EXPECT_EQ(0, datFile.AddField("dec_fld", TABFDecimal, 12, 3));
  EXPECT_EQ(0, datFile.AddField("log_fld", TABFLogical, 1));
  EXPECT_EQ(0, datFile.AddField("date_fld", TABFDate, 8));
  EXPECT_EQ(0, datFile.AddField("time_fld", TABFTime, 9));
  EXPECT_EQ(0, datFile.AddField("dt_fld", TABFDateTime, 17));

  // Test invalid width (>254) and default width mappings.
  EXPECT_EQ(-1, datFile.AddField("too_wide", TABFChar, 300));
  CPLErrorReset();
  EXPECT_EQ(0,
            datFile.AddField("auto_dec", TABFDecimal, 0, 2));  // defaults to 20
  EXPECT_EQ(0, datFile.AddField("auto_char", TABFChar, 0));  // defaults to 254
  EXPECT_EQ(-1, datFile.AddField("bad_type", TABFUnknown, 10));
  CPLErrorReset();

  EXPECT_EQ(12, datFile.GetNumFields());

  // Record 1: standard formatted strings & numbers.
  TABRawBinBlock* block = datFile.GetRecordBlock(1);
  ASSERT_THAT(block, NotNull());
  EXPECT_EQ(0, datFile.WriteCharField("Hello World", 20, &indFile, 1));
  EXPECT_EQ(0, datFile.WriteIntegerField(123456, &indFile, 2));
  EXPECT_EQ(0, datFile.WriteSmallIntField(-3200, &indFile, 3));
  EXPECT_EQ(0, datFile.WriteLargeIntField(9876543210LL, &indFile, 4));
  EXPECT_EQ(0, datFile.WriteFloatField(123.456, &indFile, 5));
  EXPECT_EQ(0, datFile.WriteDecimalField(789.123, 12, 3, &indFile, 6));
  EXPECT_EQ(0, datFile.WriteLogicalField(true, &indFile, 7));
  EXPECT_EQ(0, datFile.WriteDateField("20250515", &indFile, 8));
  EXPECT_EQ(0, datFile.WriteTimeField("14:30:45", &indFile, 9));
  // Note: poINDFile must be null for DateTime since indexing is unsupported.
  EXPECT_EQ(0, datFile.WriteDateTimeField("20250515143045123", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(12.34, 20, 2, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteCharField("auto char text", 254, nullptr, 0));
  EXPECT_EQ(0, datFile.CommitRecordToFile());

  // Record 2: alternate string formats (slashes, millis, space in datetime).
  block = datFile.GetRecordBlock(2);
  ASSERT_THAT(block, NotNull());
  EXPECT_EQ(0, datFile.WriteCharField("Short", 20, &indFile, 1));
  EXPECT_EQ(0, datFile.WriteIntegerField(0, &indFile, 2));
  EXPECT_EQ(0, datFile.WriteSmallIntField(0, &indFile, 3));
  EXPECT_EQ(0, datFile.WriteLargeIntField(0, &indFile, 4));
  EXPECT_EQ(0, datFile.WriteFloatField(0.0, &indFile, 5));
  EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 12, 3, &indFile, 6));
  EXPECT_EQ(0, datFile.WriteLogicalField(false, &indFile, 7));
  EXPECT_EQ(0, datFile.WriteDateField("2025/05/15", &indFile, 8));
  EXPECT_EQ(0, datFile.WriteTimeField("143045500", &indFile, 9));
  EXPECT_EQ(0, datFile.WriteDateTimeField("2025/05/15 14:30:45", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 20, 2, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteCharField("", 254, nullptr, 0));
  EXPECT_EQ(0, datFile.CommitRecordToFile());

  // Record 3: component overloads for date/time/datetime and mark as deleted.
  block = datFile.GetRecordBlock(3);
  ASSERT_THAT(block, NotNull());
  EXPECT_EQ(0, datFile.WriteCharField("Deleted Rec", 20, &indFile, 1));
  EXPECT_EQ(0, datFile.WriteIntegerField(999, &indFile, 2));
  EXPECT_EQ(0, datFile.WriteSmallIntField(99, &indFile, 3));
  EXPECT_EQ(0, datFile.WriteLargeIntField(9999LL, &indFile, 4));
  EXPECT_EQ(0, datFile.WriteFloatField(9.9, &indFile, 5));
  EXPECT_EQ(0, datFile.WriteDecimalField(99.99, 12, 3, &indFile, 6));
  EXPECT_EQ(0, datFile.WriteLogicalField(true, &indFile, 7));
  EXPECT_EQ(0, datFile.WriteDateField(2025, 5, 15, &indFile, 8));
  EXPECT_EQ(0, datFile.WriteTimeField(14, 30, 45, 500, &indFile, 9));
  EXPECT_EQ(
      0, datFile.WriteDateTimeField(2025, 5, 15, 14, 30, 45, 500, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 20, 2, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteCharField("", 254, nullptr, 0));
  EXPECT_EQ(0, datFile.MarkAsDeleted());
  EXPECT_EQ(0, datFile.CommitRecordToFile());

  // Record 4: date DD/MM/YYYY format, empty strings, negative time, decimal
  // overflow.
  block = datFile.GetRecordBlock(4);
  ASSERT_THAT(block, NotNull());
  EXPECT_EQ(0, datFile.WriteCharField("Rec 4", 20, &indFile, 1));
  EXPECT_EQ(0, datFile.WriteIntegerField(44, &indFile, 2));
  EXPECT_EQ(0, datFile.WriteSmallIntField(4, &indFile, 3));
  EXPECT_EQ(0, datFile.WriteLargeIntField(4444LL, &indFile, 4));
  EXPECT_EQ(0, datFile.WriteFloatField(4.4, &indFile, 5));
  // Decimal overflow: format doesn't fit width.
  EXPECT_EQ(-1, datFile.WriteDecimalField(1e25, 12, 3, &indFile, 6));
  CPLErrorReset();
  EXPECT_EQ(0, datFile.WriteDecimalField(44.44, 12, 3, &indFile, 6));
  EXPECT_EQ(0, datFile.WriteLogicalField(false, &indFile, 7));
  EXPECT_EQ(0, datFile.WriteDateField("15/05/2025", &indFile, 8));
  // Negative time components -> treated as not set (-1).
  EXPECT_EQ(0, datFile.WriteTimeField(-1, -1, -1, -1, &indFile, 9));
  EXPECT_EQ(0, datFile.WriteDateTimeField("", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 20, 2, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteCharField("", 254, nullptr, 0));
  // Mark as deleted, then restore to existing.
  EXPECT_EQ(0, datFile.MarkAsDeleted());
  EXPECT_TRUE(datFile.IsCurrentRecordDeleted());
  EXPECT_EQ(0, datFile.MarkRecordAsExisting());
  EXPECT_FALSE(datFile.IsCurrentRecordDeleted());
  EXPECT_EQ(0, datFile.CommitRecordToFile());

  // Error write calls: invalid dates/times, illegal widths.
  EXPECT_EQ(-1, datFile.WriteCharField("fail", 0, nullptr, 0));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.WriteCharField("fail", 300, nullptr, 0));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.WriteDateField("bad_date_format", nullptr, 0));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.WriteTimeField("invalid_time", nullptr, 0));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.WriteDateTimeField("bad_datetime", nullptr, 0));
  CPLErrorReset();

  // Record 5: leading spaces in date/time/datetime and DD/MM/YYYY HH:MM:SS
  // format.
  block = datFile.GetRecordBlock(5);
  ASSERT_THAT(block, NotNull());
  EXPECT_EQ(0, datFile.WriteCharField("Rec 5", 20, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteIntegerField(55, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteSmallIntField(5, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteLargeIntField(5555LL, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteFloatField(5.5, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(55.55, 12, 3, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteLogicalField(true, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDateField("   2025/05/15", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteTimeField("   14:30:45", nullptr, 0));
  EXPECT_EQ(0,
            datFile.WriteDateTimeField("   15/05/2025 14:30:45", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 20, 2, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteCharField("", 254, nullptr, 0));
  EXPECT_EQ(0, datFile.CommitRecordToFile());

  // Record 6: empty date/time/datetime strings (lines 2320-2323, 2438-2442,
  // 2575-2583).
  block = datFile.GetRecordBlock(6);
  ASSERT_THAT(block, NotNull());
  EXPECT_EQ(0, datFile.WriteCharField("Rec 6", 20, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteIntegerField(66, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteSmallIntField(6, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteLargeIntField(6666LL, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteFloatField(6.6, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(66.66, 12, 3, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteLogicalField(false, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDateField("", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteTimeField("", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDateTimeField("", nullptr, 0));
  EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 20, 2, nullptr, 0));
  EXPECT_EQ(0, datFile.WriteCharField("", 254, nullptr, 0));
  EXPECT_EQ(0, datFile.CommitRecordToFile());

  EXPECT_EQ(6, datFile.GetNumRecords());
  EXPECT_EQ(0, datFile.SyncToDisk());
  EXPECT_EQ(0, datFile.Close());
  EXPECT_EQ(0, indFile.Close());

  // 3. Reopen DAT file in TABReadWrite mode to validate fields and read back.
  TABDATFile readFile("UTF-8");
  ASSERT_EQ(0, readFile.Open(datPath.c_str(), TABReadWrite));
  EXPECT_EQ(12, readFile.GetNumFields());
  EXPECT_EQ(6, readFile.GetNumRecords());

  // Field validation: success for all 10 types.
  EXPECT_EQ(0,
            readFile.ValidateFieldInfoFromTAB(0, "char_fld", TABFChar, 20, 0));
  EXPECT_EQ(
      0, readFile.ValidateFieldInfoFromTAB(1, "int_fld", TABFInteger, 10, 0));
  EXPECT_EQ(
      0, readFile.ValidateFieldInfoFromTAB(2, "sint_fld", TABFSmallInt, 5, 0));
  EXPECT_EQ(
      0, readFile.ValidateFieldInfoFromTAB(3, "lint_fld", TABFLargeInt, 20, 0));
  EXPECT_EQ(0,
            readFile.ValidateFieldInfoFromTAB(4, "flt_fld", TABFFloat, 19, 8));
  EXPECT_EQ(
      0, readFile.ValidateFieldInfoFromTAB(5, "dec_fld", TABFDecimal, 12, 3));
  EXPECT_EQ(0,
            readFile.ValidateFieldInfoFromTAB(6, "log_fld", TABFLogical, 1, 0));
  EXPECT_EQ(0,
            readFile.ValidateFieldInfoFromTAB(7, "date_fld", TABFDate, 8, 0));
  EXPECT_EQ(0,
            readFile.ValidateFieldInfoFromTAB(8, "time_fld", TABFTime, 9, 0));
  EXPECT_EQ(
      0, readFile.ValidateFieldInfoFromTAB(9, "dt_fld", TABFDateTime, 17, 0));

  // Field validation: mismatching type / width / invalid index.
  EXPECT_EQ(
      -1, readFile.ValidateFieldInfoFromTAB(0, "char_fld", TABFInteger, 10, 0));
  CPLErrorReset();
  EXPECT_EQ(-1,
            readFile.ValidateFieldInfoFromTAB(0, "char_fld", TABFChar, 99, 0));
  CPLErrorReset();
  EXPECT_EQ(
      -1, readFile.ValidateFieldInfoFromTAB(5, "dec_fld", TABFDecimal, 12, 5));
  CPLErrorReset();
  EXPECT_EQ(-1, readFile.ValidateFieldInfoFromTAB(-1, "bad", TABFChar, 10, 0));
  CPLErrorReset();
  EXPECT_EQ(-1, readFile.ValidateFieldInfoFromTAB(50, "bad", TABFChar, 10, 0));
  CPLErrorReset();

  // Field property getters.
  EXPECT_EQ(TABFChar, readFile.GetFieldType(0));
  EXPECT_EQ(20, readFile.GetFieldWidth(0));
  EXPECT_EQ(0, readFile.GetFieldPrecision(0));
  EXPECT_EQ(TABFDecimal, readFile.GetFieldType(5));
  EXPECT_EQ(12, readFile.GetFieldWidth(5));
  EXPECT_EQ(3, readFile.GetFieldPrecision(5));
  EXPECT_EQ(TABFUnknown, readFile.GetFieldType(99));
  EXPECT_EQ(0, readFile.GetFieldWidth(99));
  EXPECT_EQ(0, readFile.GetFieldPrecision(99));

  // Read Record 1.
  block = readFile.GetRecordBlock(1);
  ASSERT_THAT(block, NotNull());
  EXPECT_FALSE(readFile.IsCurrentRecordDeleted());
  EXPECT_THAT(readFile.ReadCharField(20), StrEq("Hello World"));
  EXPECT_EQ(123456, readFile.ReadIntegerField(10));
  EXPECT_EQ(-3200, readFile.ReadSmallIntField(5));
  EXPECT_EQ(9876543210LL, readFile.ReadLargeIntField(20));
  EXPECT_NEAR(123.456, readFile.ReadFloatField(19), 1e-4);
  EXPECT_NEAR(789.123, readFile.ReadDecimalField(12), 1e-3);
  EXPECT_TRUE(readFile.ReadLogicalField(1));

  int y = 0, m = 0, d = 0;
  EXPECT_EQ(0, readFile.ReadDateField(8, &y, &m, &d));
  EXPECT_EQ(2025, y);
  EXPECT_EQ(5, m);
  EXPECT_EQ(15, d);

  int h = 0, min = 0, sec = 0, ms = 0;
  EXPECT_EQ(0, readFile.ReadTimeField(9, &h, &min, &sec, &ms));
  EXPECT_EQ(14, h);
  EXPECT_EQ(30, min);
  EXPECT_EQ(45, sec);
  EXPECT_EQ(0, ms);

  int dty = 0, dtm = 0, dtd = 0, dth = 0, dtmin = 0, dtsec = 0, dtms = 0;
  EXPECT_EQ(0, readFile.ReadDateTimeField(17, &dty, &dtm, &dtd, &dth, &dtmin,
                                          &dtsec, &dtms));
  EXPECT_EQ(2025, dty);
  EXPECT_EQ(5, dtm);
  EXPECT_EQ(15, dtd);
  EXPECT_EQ(14, dth);
  EXPECT_EQ(30, dtmin);
  EXPECT_EQ(45, dtsec);
  EXPECT_EQ(123, dtms);

  // Read Record 2 (alternate formats).
  block = readFile.GetRecordBlock(2);
  ASSERT_THAT(block, NotNull());
  EXPECT_THAT(readFile.ReadCharField(20), StrEq("Short"));
  EXPECT_EQ(0, readFile.ReadIntegerField(10));
  EXPECT_EQ(0, readFile.ReadSmallIntField(5));
  EXPECT_EQ(0LL, readFile.ReadLargeIntField(20));
  EXPECT_NEAR(0.0, readFile.ReadFloatField(19), 1e-4);
  EXPECT_NEAR(0.0, readFile.ReadDecimalField(12), 1e-3);
  EXPECT_FALSE(readFile.ReadLogicalField(1));
  EXPECT_THAT(readFile.ReadDateField(8), StrEq("20250515"));
  EXPECT_THAT(readFile.ReadTimeField(9), StrEq("143045500"));
  EXPECT_THAT(readFile.ReadDateTimeField(17), StrEq("20250515143045000"));

  // Read Record 3 (deleted record).
  block = readFile.GetRecordBlock(3);
  ASSERT_THAT(block, NotNull());
  EXPECT_TRUE(readFile.IsCurrentRecordDeleted());
  EXPECT_THAT(readFile.ReadCharField(20), StrEq(""));
  EXPECT_EQ(0, readFile.ReadIntegerField(10));
  EXPECT_EQ(0, readFile.ReadSmallIntField(5));
  EXPECT_EQ(0LL, readFile.ReadLargeIntField(20));
  EXPECT_NEAR(0.0, readFile.ReadFloatField(19), 1e-4);
  EXPECT_NEAR(0.0, readFile.ReadDecimalField(12), 1e-3);
  EXPECT_FALSE(readFile.ReadLogicalField(1));
  EXPECT_THAT(readFile.ReadDateField(8), StrEq(""));
  EXPECT_THAT(readFile.ReadTimeField(9), StrEq(""));
  EXPECT_THAT(readFile.ReadDateTimeField(17), StrEq(""));

  // Read Record 4 (restored record).
  block = readFile.GetRecordBlock(4);
  ASSERT_THAT(block, NotNull());
  EXPECT_FALSE(readFile.IsCurrentRecordDeleted());
  EXPECT_THAT(readFile.ReadCharField(20), StrEq("Rec 4"));
  EXPECT_EQ(44, readFile.ReadIntegerField(10));
  EXPECT_EQ(4, readFile.ReadSmallIntField(5));
  EXPECT_EQ(4444LL, readFile.ReadLargeIntField(20));
  EXPECT_NEAR(4.4, readFile.ReadFloatField(19), 1e-4);
  EXPECT_NEAR(44.44, readFile.ReadDecimalField(12), 1e-2);
  EXPECT_FALSE(readFile.ReadLogicalField(1));
  EXPECT_THAT(readFile.ReadDateField(8), StrEq("20250515"));
  // Time was not set (-1) -> ReadTimeField returns "".
  EXPECT_THAT(readFile.ReadTimeField(9), StrEq(""));
  // DateTime was empty -> ReadDateTimeField returns "".
  EXPECT_THAT(readFile.ReadDateTimeField(17), StrEq(""));

  // Read Record 5 (leading spaces).
  block = readFile.GetRecordBlock(5);
  ASSERT_THAT(block, NotNull());
  EXPECT_THAT(readFile.ReadCharField(20), StrEq("Rec 5"));
  EXPECT_EQ(55, readFile.ReadIntegerField(10));
  EXPECT_EQ(5, readFile.ReadSmallIntField(5));
  EXPECT_EQ(5555LL, readFile.ReadLargeIntField(20));
  EXPECT_NEAR(5.5, readFile.ReadFloatField(19), 1e-4);
  EXPECT_NEAR(55.55, readFile.ReadDecimalField(12), 1e-2);
  EXPECT_TRUE(readFile.ReadLogicalField(1));
  EXPECT_THAT(readFile.ReadDateField(8), StrEq("20250515"));
  EXPECT_THAT(readFile.ReadTimeField(9), StrEq("143045000"));
  EXPECT_THAT(readFile.ReadDateTimeField(17), StrEq("20250515143045000"));

  // Read Record 6 (all date/time empty, hitting lines 1853-1854).
  block = readFile.GetRecordBlock(6);
  ASSERT_THAT(block, NotNull());
  EXPECT_THAT(readFile.ReadCharField(20), StrEq("Rec 6"));
  EXPECT_EQ(66, readFile.ReadIntegerField(10));
  EXPECT_EQ(6, readFile.ReadSmallIntField(5));
  EXPECT_EQ(6666LL, readFile.ReadLargeIntField(20));
  EXPECT_NEAR(6.6, readFile.ReadFloatField(19), 1e-4);
  EXPECT_NEAR(66.66, readFile.ReadDecimalField(12), 1e-2);
  EXPECT_FALSE(readFile.ReadLogicalField(1));
  EXPECT_THAT(readFile.ReadDateField(8), StrEq(""));
  EXPECT_THAT(readFile.ReadTimeField(9), StrEq(""));
  EXPECT_THAT(readFile.ReadDateTimeField(17), StrEq(""));

  // Out-of-range GetRecordBlock in read-write mode.
  EXPECT_EQ(nullptr, readFile.GetRecordBlock(0));
  EXPECT_EQ(nullptr, readFile.GetRecordBlock(-1));
  CPLErrorReset();

  // Out-of-range GetRecordBlock in read-only mode.
  TABDATFile roFile("");
  ASSERT_EQ(0, roFile.Open(datPath.c_str(), TABRead));
  EXPECT_EQ(nullptr, roFile.GetRecordBlock(0));
  EXPECT_EQ(nullptr, roFile.GetRecordBlock(999));
  CPLErrorReset();
  EXPECT_EQ(0, roFile.Close());

  // Test invalid char width on a valid record block.
  EXPECT_THAT(readFile.GetRecordBlock(1), NotNull());
  EXPECT_STREQ("", readFile.ReadCharField(0));
  EXPECT_STREQ("", readFile.ReadCharField(300));
  CPLErrorReset();

  EXPECT_EQ(0, readFile.Close());
}

TEST(TABDATFileTest, DBFTableMode) {
  const std::string dbfPath = "/vsimem/dbf_test.dbf";
  absl::Cleanup cleanupDbf = [dbfPath]() { VSIUnlink(dbfPath.c_str()); };

  // Build a valid 10-field DBF file containing 1 record.
  struct FieldSpec {
    const char name[11];
    char type;
    uint8_t length;
    uint8_t decimals;
    const char val[30];
  };

  const FieldSpec fields[] = {
      {"STR_FLD", 'C', 10, 0, "ABCDE     "},
      {"INT_FLD", 'N', 10, 0, "12345     "},
      {"SINT_FLD", 'N', 5, 0, "-321 "},
      {"LINT_FLD", 'N', 18, 0, "9876543210        "},
      {"FLT_FLD", 'F', 10, 4, "123.4567  "},
      {"DEC_FLD", 'N', 10, 2, "456.78    "},
      {"LOG_FLD", 'L', 1, 0, "T"},
      {"DAT_FLD", 'D', 8, 0, "20250515"},
      {"TIM_FLD", 'C', 9, 0, "143045500"},
      {"DT_FLD", 'C', 17, 0, "20250515143045123"},
  };
  constexpr int numFields = 10;
  constexpr uint16_t headerLen = 32 + numFields * 32 + 1;

  uint16_t recordLen = 1;  // delete flag ' '
  for (int i = 0; i < numFields; ++i) {
    recordLen += fields[i].length;
  }

  std::vector<uint8_t> dbfData(headerLen + recordLen, 0);

  // DBF 32-byte header.
  dbfData[0] = 0x03;
  dbfData[1] = 125;  // YY (2025)
  dbfData[2] = 5;    // MM
  dbfData[3] = 15;   // DD
  // Num records = 1
  dbfData[4] = 1;
  dbfData[5] = 0;
  dbfData[6] = 0;
  dbfData[7] = 0;
  // Header length
  dbfData[8] = static_cast<uint8_t>(headerLen & 0xFF);
  dbfData[9] = static_cast<uint8_t>((headerLen >> 8) & 0xFF);
  // Record length
  dbfData[10] = static_cast<uint8_t>(recordLen & 0xFF);
  dbfData[11] = static_cast<uint8_t>((recordLen >> 8) & 0xFF);

  // Field descriptors (32 bytes each).
  for (int i = 0; i < numFields; ++i) {
    int offset = 32 + i * 32;
    std::memcpy(&dbfData[offset], fields[i].name, std::strlen(fields[i].name));
    dbfData[offset + 11] = fields[i].type;
    dbfData[offset + 16] = fields[i].length;
    dbfData[offset + 17] = fields[i].decimals;
  }
  // Terminator byte.
  dbfData[headerLen - 1] = 0x0D;

  // Record 1.
  int recOffset = headerLen;
  dbfData[recOffset] = ' ';  // active record
  int curFieldOffset = recOffset + 1;
  for (int i = 0; i < numFields; ++i) {
    std::memcpy(&dbfData[curFieldOffset], fields[i].val, fields[i].length);
    curFieldOffset += fields[i].length;
  }

  // Write DBF to vsimem.
  VSILFILE* fp = VSIFOpenL(dbfPath.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  VSIFWriteL(dbfData.data(), 1, dbfData.size(), fp);
  VSIFCloseL(fp);

  // Open in TABTableDBF mode.
  TABDATFile dbfFile("");
  ASSERT_EQ(0, dbfFile.Open(dbfPath.c_str(), "r", TABTableDBF));
  EXPECT_EQ(10, dbfFile.GetNumFields());
  EXPECT_EQ(1, dbfFile.GetNumRecords());

  // ValidateFieldInfoFromTAB for DBF maps DBF fields to TAB types.
  EXPECT_EQ(0, dbfFile.ValidateFieldInfoFromTAB(0, "STR_FLD", TABFChar, 10, 0));
  EXPECT_EQ(0,
            dbfFile.ValidateFieldInfoFromTAB(1, "INT_FLD", TABFInteger, 10, 0));
  EXPECT_EQ(
      0, dbfFile.ValidateFieldInfoFromTAB(2, "SINT_FLD", TABFSmallInt, 5, 0));
  EXPECT_EQ(
      0, dbfFile.ValidateFieldInfoFromTAB(3, "LINT_FLD", TABFLargeInt, 18, 0));
  EXPECT_EQ(0,
            dbfFile.ValidateFieldInfoFromTAB(4, "FLT_FLD", TABFFloat, 10, 4));
  EXPECT_EQ(0,
            dbfFile.ValidateFieldInfoFromTAB(5, "DEC_FLD", TABFDecimal, 10, 2));
  EXPECT_EQ(0,
            dbfFile.ValidateFieldInfoFromTAB(6, "LOG_FLD", TABFLogical, 1, 0));
  EXPECT_EQ(0, dbfFile.ValidateFieldInfoFromTAB(7, "DAT_FLD", TABFDate, 8, 0));
  EXPECT_EQ(0, dbfFile.ValidateFieldInfoFromTAB(8, "TIM_FLD", TABFTime, 9, 0));
  EXPECT_EQ(0,
            dbfFile.ValidateFieldInfoFromTAB(9, "DT_FLD", TABFDateTime, 17, 0));

  // Read record in DBF mode.
  TABRawBinBlock* block = dbfFile.GetRecordBlock(1);
  ASSERT_THAT(block, NotNull());
  EXPECT_FALSE(dbfFile.IsCurrentRecordDeleted());

  EXPECT_THAT(dbfFile.ReadCharField(10), StrEq("ABCDE"));
  EXPECT_EQ(12345, dbfFile.ReadIntegerField(10));
  EXPECT_EQ(-321, dbfFile.ReadSmallIntField(5));
  EXPECT_EQ(9876543210LL, dbfFile.ReadLargeIntField(18));
  EXPECT_NEAR(123.4567, dbfFile.ReadFloatField(10), 1e-3);
  EXPECT_NEAR(456.78, dbfFile.ReadDecimalField(10), 1e-2);
  EXPECT_TRUE(dbfFile.ReadLogicalField(1));
#if !defined(ABSL_HAVE_ADDRESS_SANITIZER)
  // Upstream bug in TABDATFile::ReadDateField/ReadTimeField/ReadDateTimeField
  // for DBF files: strcpy(m_szBuffer, ReadCharField(nWidth)) where
  // ReadCharField returns m_szBuffer, triggering strcpy-param-overlap under
  // ASan.
  EXPECT_THAT(dbfFile.ReadDateField(8), StrEq("20250515"));
  EXPECT_THAT(dbfFile.ReadTimeField(9), StrEq("000000000"));
  EXPECT_THAT(dbfFile.ReadDateTimeField(17), StrEq("20250515000000000"));
#else
  EXPECT_THAT(dbfFile.ReadCharField(8), StrEq("20250515"));
  EXPECT_THAT(dbfFile.ReadCharField(9), StrEq("143045500"));
  EXPECT_THAT(dbfFile.ReadCharField(17), StrEq("20250515143045123"));
#endif

  // Modifications not supported on non-native (DBF) table.
  EXPECT_EQ(-1, dbfFile.AddField("new", TABFChar, 10));
  CPLErrorReset();
  EXPECT_EQ(-1, dbfFile.DeleteField(0));
  CPLErrorReset();
  int dbfPerm[10] = {0};
  EXPECT_EQ(-1, dbfFile.ReorderFields(dbfPerm));
  CPLErrorReset();
  OGRFieldDefn srcD("STR_FLD", OFTString);
  OGRFieldDefn newD("NEW_STR", OFTString);
  EXPECT_EQ(-1, dbfFile.AlterFieldDefn(0, &srcD, &newD, ALTER_ALL_FLAG));
  CPLErrorReset();

  EXPECT_EQ(0, dbfFile.Close());
}

TEST(TABDATFileTest, SchemaMutationsEmptyTable) {
  const std::string emptyPath = "/vsimem/schema_empty.dat";
  absl::Cleanup cleanupEmpty = [emptyPath]() { VSIUnlink(emptyPath.c_str()); };

  TABDATFile datFile("");
  ASSERT_EQ(0, datFile.Open(emptyPath.c_str(), TABWrite));

  // Add 3 fields to empty table.
  EXPECT_EQ(0, datFile.AddField("col_a", TABFChar, 10));
  EXPECT_EQ(0, datFile.AddField("col_b", TABFInteger, 10));
  EXPECT_EQ(0, datFile.AddField("col_c", TABFDecimal, 12, 3));
  EXPECT_EQ(3, datFile.GetNumFields());

  // Reorder fields on empty table.
  int permEmpty[3] = {2, 0, 1};
  EXPECT_EQ(0, datFile.ReorderFields(permEmpty));
  EXPECT_THAT(datFile.GetFieldWidth(0), Eq(12));
  EXPECT_THAT(datFile.GetFieldWidth(1), Eq(10));
  EXPECT_THAT(datFile.GetFieldWidth(2), Eq(4));

  // AlterFieldDefn on empty table (rename, change width/precision, change
  // type).
  OGRFieldDefn srcDefn("old_name", OFTReal);
  srcDefn.SetWidth(12);
  srcDefn.SetPrecision(3);
  OGRFieldDefn newDefn("new_col", OFTString);
  newDefn.SetWidth(25);
  EXPECT_EQ(0, datFile.AlterFieldDefn(0, &srcDefn, &newDefn, ALTER_ALL_FLAG));
  EXPECT_EQ(TABFChar, datFile.GetFieldType(0));
  EXPECT_EQ(25, datFile.GetFieldWidth(0));

  // Delete field on empty table.
  EXPECT_EQ(0, datFile.DeleteField(1));
  EXPECT_EQ(2, datFile.GetNumFields());
  EXPECT_EQ(0, datFile.DeleteField(0));
  EXPECT_EQ(1, datFile.GetNumFields());

  // Error: invalid delete index.
  EXPECT_EQ(-1, datFile.DeleteField(5));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.DeleteField(-1));
  CPLErrorReset();

  // AlterFieldDefn error branches on empty table.
  EXPECT_EQ(-1, datFile.AlterFieldDefn(-1, &srcDefn, &newDefn, ALTER_ALL_FLAG));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.AlterFieldDefn(5, &srcDefn, &newDefn, ALTER_ALL_FLAG));
  CPLErrorReset();

  OGRFieldDefn badTypeDefn("bad", OFTBinary);
  EXPECT_EQ(-1,
            datFile.AlterFieldDefn(0, &srcDefn, &badTypeDefn, ALTER_TYPE_FLAG));
  CPLErrorReset();
  EXPECT_EQ(-1, datFile.AlterFieldDefn(0, &srcDefn, &badTypeDefn,
                                       ALTER_WIDTH_PRECISION_FLAG));
  CPLErrorReset();

  // AlterFieldDefn on decimal field in empty table (lines 1323-1327).
  EXPECT_EQ(0, datFile.AddField("col_dec", TABFDecimal, 10, 2));
  OGRFieldDefn decSrc("col_dec", OFTReal);
  decSrc.SetWidth(10);
  decSrc.SetPrecision(2);
  OGRFieldDefn decNew("col_dec_new", OFTReal);
  decNew.SetWidth(14);
  decNew.SetPrecision(4);
  EXPECT_EQ(0, datFile.AlterFieldDefn(1, &decSrc, &decNew,
                                      ALTER_WIDTH_PRECISION_FLAG));
  EXPECT_EQ(14, datFile.GetFieldWidth(1));
  EXPECT_EQ(4, datFile.GetFieldPrecision(1));

  // AlterFieldDefn with only ALTER_TYPE_FLAG to String (no width flag) on empty
  // table preserves width.
  OGRFieldDefn strNoWidth("col_str", OFTString);
  EXPECT_EQ(0,
            datFile.AlterFieldDefn(1, &decSrc, &strNoWidth, ALTER_TYPE_FLAG));
  EXPECT_EQ(14, datFile.GetFieldWidth(1));

  // ReorderFields with invalid permutation.
  int badPerm[2] = {0, 0};
  EXPECT_EQ(-1, datFile.ReorderFields(badPerm));
  CPLErrorReset();

  // Delete remaining fields to test m_numFields == 0 in ReorderFields.
  EXPECT_EQ(0, datFile.DeleteField(0));
  EXPECT_EQ(0, datFile.DeleteField(0));
  EXPECT_EQ(0, datFile.GetNumFields());
  EXPECT_EQ(0, datFile.ReorderFields(nullptr));

  EXPECT_EQ(0, datFile.Close());
}

TEST(TABDATFileTest, SchemaMutationsPopulatedTable) {
  const std::string popPath = "/vsimem/schema_pop.dat";
  absl::Cleanup cleanupPop = [popPath]() { VSIUnlink(popPath.c_str()); };

  // 1. Create table with all 10 field types and populate with 2 records.
  {
    TABDATFile datFile("");
    ASSERT_EQ(0, datFile.Open(popPath.c_str(), TABWrite));

    EXPECT_EQ(0, datFile.AddField("f_char", TABFChar, 20));
    EXPECT_EQ(0, datFile.AddField("f_int", TABFInteger, 10));
    EXPECT_EQ(0, datFile.AddField("f_sint", TABFSmallInt, 5));
    EXPECT_EQ(0, datFile.AddField("f_lint", TABFLargeInt, 20));
    EXPECT_EQ(0, datFile.AddField("f_flt", TABFFloat, 19, 8));
    EXPECT_EQ(0, datFile.AddField("f_dec", TABFDecimal, 12, 3));
    EXPECT_EQ(0, datFile.AddField("f_log", TABFLogical, 1));
    EXPECT_EQ(0, datFile.AddField("f_date", TABFDate, 8));
    EXPECT_EQ(0, datFile.AddField("f_time", TABFTime, 9));
    EXPECT_EQ(0, datFile.AddField("f_dt", TABFDateTime, 17));

    // Record 1: active.
    TABRawBinBlock* blk = datFile.GetRecordBlock(1);
    ASSERT_THAT(blk, NotNull());
    EXPECT_EQ(0, datFile.WriteCharField("ABC", 20, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteIntegerField(100, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteSmallIntField(20, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteLargeIntField(300LL, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteFloatField(4.5, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteDecimalField(5.6, 12, 3, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteLogicalField(true, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteDateField("20250515", nullptr, 0));
    EXPECT_EQ(0, datFile.WriteTimeField("120000000", nullptr, 0));
    EXPECT_EQ(0, datFile.WriteDateTimeField("20250515120000000", nullptr, 0));
    EXPECT_EQ(0, datFile.CommitRecordToFile());

    // Record 2: deleted.
    blk = datFile.GetRecordBlock(2);
    ASSERT_THAT(blk, NotNull());
    EXPECT_EQ(0, datFile.WriteCharField("DEL", 20, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteIntegerField(0, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteSmallIntField(0, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteLargeIntField(0LL, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteFloatField(0.0, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteDecimalField(0.0, 12, 3, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteLogicalField(false, nullptr, 0));
    EXPECT_EQ(0, datFile.WriteDateField("20250515", nullptr, 0));
    EXPECT_EQ(0, datFile.WriteTimeField("120000000", nullptr, 0));
    EXPECT_EQ(0, datFile.WriteDateTimeField("20250515120000000", nullptr, 0));
    EXPECT_EQ(0, datFile.MarkAsDeleted());
    EXPECT_EQ(0, datFile.CommitRecordToFile());

    EXPECT_EQ(0, datFile.Close());
  }

  // 2. Reopen in TABReadWrite and alter all 10 types to OFTString to hit lines
  // 1420-1475.
  {
    TABDATFile datFile("");
    ASSERT_EQ(0, datFile.Open(popPath.c_str(), TABReadWrite));

    EXPECT_EQ(0,
              datFile.ValidateFieldInfoFromTAB(0, "f_char", TABFChar, 20, 0));
    EXPECT_EQ(0,
              datFile.ValidateFieldInfoFromTAB(1, "f_int", TABFInteger, 10, 0));
    EXPECT_EQ(
        0, datFile.ValidateFieldInfoFromTAB(2, "f_sint", TABFSmallInt, 5, 0));
    EXPECT_EQ(
        0, datFile.ValidateFieldInfoFromTAB(3, "f_lint", TABFLargeInt, 20, 0));
    EXPECT_EQ(0,
              datFile.ValidateFieldInfoFromTAB(4, "f_flt", TABFFloat, 19, 8));
    EXPECT_EQ(0,
              datFile.ValidateFieldInfoFromTAB(5, "f_dec", TABFDecimal, 12, 3));
    EXPECT_EQ(0,
              datFile.ValidateFieldInfoFromTAB(6, "f_log", TABFLogical, 1, 0));
    EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(7, "f_date", TABFDate, 8, 0));
    EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(8, "f_time", TABFTime, 9, 0));
    EXPECT_EQ(0,
              datFile.ValidateFieldInfoFromTAB(9, "f_dt", TABFDateTime, 17, 0));

    // Test AlterFieldDefn: rename only.
    OGRFieldDefn srcRename("f_char", OFTString);
    srcRename.SetWidth(20);
    OGRFieldDefn newRename("f_char_renamed", OFTString);
    newRename.SetWidth(20);
    EXPECT_EQ(
        0, datFile.AlterFieldDefn(0, &srcRename, &newRename, ALTER_NAME_FLAG));

    // Test AlterFieldDefn: no-op (type and width preserved).
    EXPECT_EQ(
        0, datFile.AlterFieldDefn(0, &newRename, &newRename, ALTER_ALL_FLAG));

    // Test AlterFieldDefn: error altering non-char field width on non-empty
    // layer.
    OGRFieldDefn srcInt("f_int", OFTInteger);
    srcInt.SetWidth(10);
    OGRFieldDefn badInt("f_int", OFTInteger);
    badInt.SetWidth(15);
    EXPECT_EQ(-1, datFile.AlterFieldDefn(1, &srcInt, &badInt,
                                         ALTER_WIDTH_PRECISION_FLAG));
    CPLErrorReset();

    // Test AlterFieldDefn: unsupported type conversion on populated table.
    OGRFieldDefn badType("f_int", OFTReal);
    EXPECT_EQ(-1,
              datFile.AlterFieldDefn(1, &srcInt, &badType, ALTER_TYPE_FLAG));
    CPLErrorReset();

    // Convert all 10 fields to OFTString sequentially:
    // Field 0: char -> string (wider)
    OGRFieldDefn s0("f_char_renamed", OFTString);
    s0.SetWidth(20);
    OGRFieldDefn n0("s0", OFTString);
    n0.SetWidth(25);
    EXPECT_EQ(0, datFile.AlterFieldDefn(0, &s0, &n0, ALTER_ALL_FLAG));

    // Field 1: int -> string
    OGRFieldDefn s1("f_int", OFTInteger);
    s1.SetWidth(10);
    OGRFieldDefn n1("s1", OFTString);
    n1.SetWidth(20);
    EXPECT_EQ(0, datFile.AlterFieldDefn(1, &s1, &n1, ALTER_ALL_FLAG));

    // Field 2: sint -> string
    OGRFieldDefn s2("f_sint", OFTInteger);
    s2.SetWidth(5);
    OGRFieldDefn n2("s2", OFTString);
    n2.SetWidth(15);
    EXPECT_EQ(0, datFile.AlterFieldDefn(2, &s2, &n2, ALTER_ALL_FLAG));

    // Field 3: lint -> string
    OGRFieldDefn s3("f_lint", OFTInteger64);
    s3.SetWidth(20);
    OGRFieldDefn n3("s3", OFTString);
    n3.SetWidth(25);
    EXPECT_EQ(0, datFile.AlterFieldDefn(3, &s3, &n3, ALTER_ALL_FLAG));

    // Field 4: flt -> string
    OGRFieldDefn s4("f_flt", OFTReal);
    s4.SetWidth(19);
    s4.SetPrecision(8);
    OGRFieldDefn n4("s4", OFTString);
    n4.SetWidth(30);
    EXPECT_EQ(0, datFile.AlterFieldDefn(4, &s4, &n4, ALTER_ALL_FLAG));

    // Field 5: dec -> string
    OGRFieldDefn s5("f_dec", OFTReal);
    s5.SetWidth(12);
    s5.SetPrecision(3);
    OGRFieldDefn n5("s5", OFTString);
    n5.SetWidth(30);
    EXPECT_EQ(0, datFile.AlterFieldDefn(5, &s5, &n5, ALTER_ALL_FLAG));

    // Field 6: log -> string
    OGRFieldDefn s6("f_log", OFTInteger);
    s6.SetWidth(1);
    OGRFieldDefn n6("s6", OFTString);
    n6.SetWidth(5);
    EXPECT_EQ(0, datFile.AlterFieldDefn(6, &s6, &n6, ALTER_ALL_FLAG));

    // Field 7: date -> string
    OGRFieldDefn s7("f_date", OFTDate);
    s7.SetWidth(8);
    OGRFieldDefn n7("s7", OFTString);
    n7.SetWidth(15);
    EXPECT_EQ(0, datFile.AlterFieldDefn(7, &s7, &n7, ALTER_ALL_FLAG));

    // Field 8: time -> string
    OGRFieldDefn s8("f_time", OFTTime);
    s8.SetWidth(9);
    OGRFieldDefn n8("s8", OFTString);
    n8.SetWidth(15);
    EXPECT_EQ(0, datFile.AlterFieldDefn(8, &s8, &n8, ALTER_ALL_FLAG));

    // Field 9: dt -> string
    OGRFieldDefn s9("f_dt", OFTDateTime);
    s9.SetWidth(17);
    OGRFieldDefn n9("s9", OFTString);
    n9.SetWidth(25);
    EXPECT_EQ(0, datFile.AlterFieldDefn(9, &s9, &n9, ALTER_ALL_FLAG));

    // AddField on populated table.
    EXPECT_EQ(0, datFile.AddField("new_appended", TABFChar, 10));
    EXPECT_EQ(11, datFile.GetNumFields());

    // ReorderFields on populated table.
    std::vector<int> perm(11);
    for (int i = 0; i < 11; ++i) {
      perm[i] = 10 - i;  // reverse
    }
    EXPECT_EQ(0, datFile.ReorderFields(perm.data()));

    // DeleteField on populated table: middle field.
    EXPECT_EQ(0, datFile.DeleteField(5));
    EXPECT_EQ(10, datFile.GetNumFields());

    // DeleteField on populated table: first field.
    EXPECT_EQ(0, datFile.DeleteField(0));
    EXPECT_EQ(9, datFile.GetNumFields());

    // DeleteField on populated table: last field.
    EXPECT_EQ(0, datFile.DeleteField(8));
    EXPECT_EQ(8, datFile.GetNumFields());

    EXPECT_EQ(0, datFile.Close());
  }

  // 3. Test DeleteField error when only 1 field remains.
  const std::string singlePath = "/vsimem/schema_single.dat";
  absl::Cleanup cleanupSingle = [singlePath]() {
    VSIUnlink(singlePath.c_str());
  };
  {
    TABDATFile datFile("");
    ASSERT_EQ(0, datFile.Open(singlePath.c_str(), TABWrite));
    EXPECT_EQ(0, datFile.AddField("only_fld", TABFChar, 10));
    TABRawBinBlock* blk = datFile.GetRecordBlock(1);
    ASSERT_THAT(blk, NotNull());
    EXPECT_EQ(0, datFile.WriteCharField("X", 10, nullptr, 0));
    EXPECT_EQ(0, datFile.CommitRecordToFile());
    EXPECT_EQ(0, datFile.Close());

    ASSERT_EQ(0, datFile.Open(singlePath.c_str(), TABReadWrite));
    EXPECT_EQ(-1, datFile.DeleteField(0));
    CPLErrorReset();
    EXPECT_EQ(0, datFile.Close());
  }
}

TEST(TABDATFileTest, Dump) {
  const std::string dumpPath = "/vsimem/dump_test.dat";
  absl::Cleanup cleanupDump = [dumpPath]() { VSIUnlink(dumpPath.c_str()); };

  TABDATFile datFile("");
#ifdef DEBUG
  // Dump on unopened file.
  datFile.Dump(nullptr);
#endif

  ASSERT_EQ(0, datFile.Open(dumpPath.c_str(), TABWrite));
  EXPECT_EQ(0, datFile.AddField("col", TABFChar, 10));

#ifdef DEBUG
  // Dump on opened file.
  datFile.Dump(nullptr);
#endif

  EXPECT_EQ(0, datFile.Close());
}

}  // namespace
}  // namespace autotest2
