// Tests for GDAL's C String List (CSL) API.
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_csv.cpp
//
// Copyright 2016 Google Inc. All Rights Reserved.
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

// Both of these includes define functions in cpl_csv.cpp.
#include "third_party/gdal/port/cpl_csv.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_multiproc.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/gdal_csv.h"

using testing::EndsWith;
using testing::IsNull;
using testing::NotNull;
using testing::StartsWith;

namespace autotest2 {
namespace {

// TODO(schwehr): Test CSVDeaccess.
// TODO(schwehr): Test CSVDetectSeperator.

TEST(CplCsvTest, CSVDetectSeperator) {
  // Default to comma.
  // nullptr not allowed as an arg.
  EXPECT_EQ(',', CSVDetectSeperator(""));
  EXPECT_EQ(',', CSVDetectSeperator("a"));
  EXPECT_EQ(',', CSVDetectSeperator("a\na"));

  EXPECT_EQ(',', CSVDetectSeperator("a,b;c\td e"));

  EXPECT_EQ(';', CSVDetectSeperator(";"));
  EXPECT_EQ(';', CSVDetectSeperator("a;b"));
  EXPECT_EQ(';', CSVDetectSeperator(";b"));
  EXPECT_EQ(';', CSVDetectSeperator("a\nb;c"));
  EXPECT_EQ(',', CSVDetectSeperator("a;b,c\td e"));
}

// TODO(schwehr): Test CSVReadParseLineL.

TEST(CplCsvTest, CSVReadParseLine2LEmpty) {
  const char filename[] = "/vsimem/CSVReadParseLine2L.csv";

  // Create an empty file.
  {
    VSILFILE* file = VSIFOpenL(filename, "wb");
    VSIFCloseL(file);
  }

  VSILFILE* file = VSIFOpenL(filename, "rb");

  StringListPtr fields(CSVReadParseLine2L(file, ','), CSLDestroy);
  EXPECT_THAT(fields, IsNull());

  VSIFCloseL(file);
  VSIUnlink(filename);
}

TEST(CplCsvTest, CSVReadParseLine2LOneLine) {
  const char filename[] = "/vsimem/CSVReadParseLine2L.csv";

  VsiMemTempWrapper tmp_file(filename, "a,b,\n");

  VSILFILE* file = VSIFOpenL(filename, "rb");
  {
    StringListPtr fields(CSVReadParseLine2L(file, ','), CSLDestroy);
    ASSERT_THAT(fields, NotNull());
    const auto values = CslToVector(fields.get());
    EXPECT_THAT(values, testing::ElementsAre("a", "b", ""));
  }

  // Try a delimiter that is not present.
  VSIRewindL(file);
  StringListPtr fields(CSVReadParseLine2L(file, ' '), CSLDestroy);
  ASSERT_THAT(fields, NotNull());
  const auto values = CslToVector(fields.get());
  EXPECT_THAT(values, testing::ElementsAre("a,b,"));

  VSIFCloseL(file);
}

// TODO(schwehr): Test CSVReadParseLine2L quote parsing.
// TODO(schwehr): Test CSVScanLinesL.
// TODO(schwehr): Test CSVScanLinesIndexed.
// TODO(schwehr): Test CSVScanLinesIngested.
// TODO(schwehr): Test CSVGetNextLine.
// TODO(schwehr): Test CSVScanFile.

// Do not test deprecated CSVGetFieldId (no VSI support).

TEST(CplCsvTest, CSVGetFieldIdLBasic) {
  const char filename[] = "/vsimem/CSVGetFieldIdLBasic.csv";

  const char data[] = "a,b,c\n";
  VsiMemTempWrapper tmp_file(filename, data);

  VSILFILE* file = VSIFOpenL(filename, "rb");
  EXPECT_EQ(0, CSVGetFieldIdL(file, "a"));
  EXPECT_EQ(1, CSVGetFieldIdL(file, "b"));
  EXPECT_EQ(2, CSVGetFieldIdL(file, "c"));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "DoesNotExist"));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "a "));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, ""));
  VSIFCloseL(file);
}

TEST(CplCsvTest, CSVGetFieldIdLEmptyOrSmale) {
  const char filename[] = "/vsimem/CSVGetFieldIdLEmptyOrSpace.csv";

  const char data[] = " ,,abc \n";
  VsiMemTempWrapper tmp_file(filename, data);

  VSILFILE* file = VSIFOpenL(filename, "rb");
  EXPECT_EQ(0, CSVGetFieldIdL(file, " "));
  EXPECT_EQ(1, CSVGetFieldIdL(file, ""));
  EXPECT_EQ(2, CSVGetFieldIdL(file, "abc "));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "abc"));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "a"));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "c "));
  VSIFCloseL(file);
}

// TODO(schwehr): Test CSVGetFieldIdL with invalid CSV.

TEST(CplCsvTest, CSVGetFileFieldIdBasic) {
  const char filename[] = "/vsimem/CSVGetFieldIdBasic.csv";

  const char data[] = "a,b,c\n";
  VsiMemTempWrapper tmp_file(filename, data);

  EXPECT_EQ(0, CSVGetFileFieldId(filename, "a"));
  EXPECT_EQ(1, CSVGetFileFieldId(filename, "b"));
  EXPECT_EQ(2, CSVGetFileFieldId(filename, "c"));
  EXPECT_EQ(-1, CSVGetFileFieldId(filename, "DoesNotExist"));
  EXPECT_EQ(-1, CSVGetFileFieldId(filename, "a "));
  EXPECT_EQ(-1, CSVGetFileFieldId(filename, ""));
}

TEST(CplCsvTest, CSVGetFileFieldIdMore) {
  const char filename[] = "/vsimem/CSVGetFieldIdMore.csv";

  const char data[] = "1,\"2\",\"c\",with space,-,#,\n";
  VsiMemTempWrapper tmp_file(filename, data);

  EXPECT_EQ(0, CSVGetFileFieldId(filename, "1"));
  EXPECT_EQ(1, CSVGetFileFieldId(filename, "2"));
  EXPECT_EQ(2, CSVGetFileFieldId(filename, "c"));
  EXPECT_EQ(3, CSVGetFileFieldId(filename, "with space"));
  EXPECT_EQ(4, CSVGetFileFieldId(filename, "-"));
  EXPECT_EQ(5, CSVGetFileFieldId(filename, "#"));
  EXPECT_EQ(6, CSVGetFileFieldId(filename, ""));
}

TEST(CplCsvTest, CSVGetFileFieldIdEmptyAndSpace) {
  const char filename[] = "/vsimem/CSVGetFieldIdEmptyAndSpace.csv";

  const char data[] = ", \n";
  VsiMemTempWrapper tmp_file(filename, data);

  EXPECT_EQ(0, CSVGetFileFieldId(filename, ""));
  EXPECT_EQ(1, CSVGetFileFieldId(filename, " "));
}

// TODO(schwehr): Test CSVGetFileFieldId with invalid CSV.
// TODO(schwehr): Test CSVScanFileByName.
// TODO(schwehr): Test CSVGetField.

TEST(CplCsvTest, GDALDefaultCSVFilename) {
  // Files that do not exist.
  // nullptr not allowed as an arg.
  EXPECT_STREQ("/vsimem/gdal_data/", GDALDefaultCSVFilename(""));
  EXPECT_STREQ("does_not_exist", GDALDefaultCSVFilename("does_not_exist"));
  EXPECT_STREQ("/foo/bar.csv", GDALDefaultCSVFilename("/foo/bar.csv"));
  EXPECT_STREQ("../baz.csv", GDALDefaultCSVFilename("../baz.csv"));

  // Try a file that GDAL does have.
  EXPECT_STREQ("/vsimem/gdal_data/ozi_datum.csv",
               GDALDefaultCSVFilename("ozi_datum.csv"));
  EXPECT_STREQ("/vsimem/gdal_data/ozi_datum.csv",
               GDALDefaultCSVFilename("/vsimem/gdal_data/ozi_datum.csv"));
  // Malformed attempt to find an existing file.
  // Does not find the file.
  EXPECT_STREQ("gdal_data/ozi_datum.csv",
               GDALDefaultCSVFilename("gdal_data/ozi_datum.csv"));

  // GDAL specifically searches for this file that can cause trouble.
  EXPECT_STREQ("datum.csv", GDALDefaultCSVFilename("datum.csv"));

  // Use full path in the memory filesystem.
  const char filename[] = "/vsimem/somefilename.txt";

  // Does not exist.
  EXPECT_STREQ(filename, GDALDefaultCSVFilename(filename));

  VsiMemTempWrapper tmp_file(filename, "foo");

  EXPECT_THAT(GDALDefaultCSVFilename(filename), StartsWith("/"));
  EXPECT_THAT(GDALDefaultCSVFilename(filename), EndsWith(filename));
}

TEST(CplCsvTest, DetectSeparatorAndParseEdgeCases) {
  WithQuietHandler quiet;

  // Detect pipe, tab, space, and quoted strings with doubled quotes
  EXPECT_EQ('|', CSVDetectSeperator("a|b|c,d"));
  EXPECT_EQ('\t', CSVDetectSeperator("a\tb\tc,d"));
  EXPECT_EQ(' ', CSVDetectSeperator("a b c"));
  EXPECT_EQ('|', CSVDetectSeperator("\"a\"\"b,c,d\"|e|f"));

  // CSVReadParseLine2L with nullptr
  EXPECT_EQ(nullptr, CSVReadParseLine2L(nullptr, ','));

  // Test CSVReadParseLine3L: BOM, bHonourStrings=false,
  // bKeepLeadingAndClosingQuotes=true, bMergeDelimiter=true, quotes in middle
  // of field, doubled quotes, multiline quotes, empty line, and unbalanced
  // quotes.
  const char kParseFile[] = "/vsimem/csv_parse_edges.csv";
  const std::string content =
      "\xEF\xBB\xBF"
      "a,,b\n"
      "\"x,y\",z\n"
      "\"keep\",1,50\"N,\"he\"\"llo\"\n"
      "\"multi\nline\",end\n"
      "\n"
      "\"unbalanced_eof";
  VsiMemTempWrapper wrapper(kParseFile, content);

  VSILFILE* fp = VSIFOpenL(kParseFile, "rb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { VSIFCloseL(fp); };

  // Line 1: BOM + merge delimiters + nMaxLineSize > 0
  char** l1 = CSVReadParseLine3L(fp, 1024, ",", true, false, true, true);
  ASSERT_NE(nullptr, l1);
  EXPECT_EQ(2, CSLCount(l1));
  EXPECT_STREQ("a", l1[0]);
  EXPECT_STREQ("b", l1[1]);
  CSLDestroy(l1);

  // Line 2: bHonourStrings = false
  char** l2 = CSVReadParseLine3L(fp, 0, ",", false, false, false, false);
  ASSERT_NE(nullptr, l2);
  EXPECT_EQ(3, CSLCount(l2));
  CSLDestroy(l2);

  // Line 3: bKeepLeadingAndClosingQuotes = true, quote in middle, doubled
  // quotes
  char** l3 = CSVReadParseLine3L(fp, 0, ",", true, true, false, false);
  ASSERT_NE(nullptr, l3);
  EXPECT_EQ(4, CSLCount(l3));
  EXPECT_STREQ("\"keep\"", l3[0]);
  EXPECT_STREQ("50\"N", l3[2]);
  EXPECT_STREQ("\"he\"llo\"", l3[3]);
  CSLDestroy(l3);

  // Line 4: multiline quoted string
  char** l4 = CSVReadParseLine3L(fp, 0, ",", true, false, false, false);
  ASSERT_NE(nullptr, l4);
  EXPECT_EQ(2, CSLCount(l4));
  EXPECT_STREQ("multi\nline", l4[0]);
  EXPECT_STREQ("end", l4[1]);
  CSLDestroy(l4);

  // Line 5: empty line
  char** l5 = CSVReadParseLine3L(fp, 0, ",", true, false, false, false);
  ASSERT_NE(nullptr, l5);
  EXPECT_EQ(0, CSLCount(l5));
  CSLDestroy(l5);

  // Line 6: unbalanced quotes at EOF -> nullptr
  EXPECT_EQ(nullptr, CSVReadParseLine3L(fp, 0, ",", true, false, false, false));
}

TEST(CplCsvTest, ClassicFileApisAndScanLines) {
  const std::string path = ::testing::TempDir() + "/cpl_csv_classic.csv";
  FILE* fp = fopen(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup cleanup = [&] {
    fclose(fp);
    VSIUnlink(path.c_str());
  };

  EXPECT_EQ(nullptr, CSVReadParseLine2(nullptr, ','));

  const char* csv_text =
      "id,name,score\n"
      "only_one_field\n"
      "10,Alpha,100\n"
      "20,Beta,200\n";
  fwrite(csv_text, 1, strlen(csv_text), fp);
  fflush(fp);

  EXPECT_EQ(1, CSVGetFieldId(fp, "NAME"));
  EXPECT_EQ(-1, CSVGetFieldId(fp, "missing"));

  // Rewind and skip header to test CSVScanLines
  VSIRewind(fp);
  char** hdr = CSVReadParseLine(fp);
  CSLDestroy(hdr);

  char** row_exact = CSVScanLines(fp, 1, "Beta", CC_ExactString);
  ASSERT_NE(nullptr, row_exact);
  EXPECT_STREQ("20", row_exact[0]);
  CSLDestroy(row_exact);

  VSIRewind(fp);
  CSLDestroy(CSVReadParseLine(fp));
  char** row_int = CSVScanLines(fp, 0, "10", CC_Integer);
  ASSERT_NE(nullptr, row_int);
  EXPECT_STREQ("Alpha", row_int[1]);
  CSLDestroy(row_int);

  VSIRewind(fp);
  CSLDestroy(CSVReadParseLine(fp));
  EXPECT_EQ(nullptr, CSVScanLines(fp, 1, "NonExistent", CC_ApproxString));

  // Also test CSVScanLinesL on VSILFILE
  VSILFILE* vfp = VSIFOpenL(path.c_str(), "rb");
  ASSERT_NE(nullptr, vfp);
  CSLDestroy(CSVReadParseLineL(vfp));
  char** vrow1 = CSVScanLinesL(vfp, 1, "beta", CC_ApproxString);
  ASSERT_NE(nullptr, vrow1);
  EXPECT_STREQ("20", vrow1[0]);
  CSLDestroy(vrow1);

  VSIRewindL(vfp);
  CSLDestroy(CSVReadParseLineL(vfp));
  char** vrow2 = CSVScanLinesL(vfp, 0, "20", CC_Integer);
  ASSERT_NE(nullptr, vrow2);
  EXPECT_STREQ("Beta", vrow2[1]);
  CSLDestroy(vrow2);

  VSIRewindL(vfp);
  CSLDestroy(CSVReadParseLineL(vfp));
  EXPECT_EQ(nullptr, CSVScanLinesL(vfp, 1, "Missing", CC_ExactString));
  VSIFCloseL(vfp);
}

const char* CustomCSVHook(const char* basename) {
  return CPLSPrintf("/custom_hook/%s", basename);
}

TEST(CplCsvTest, IngestedIndexedAndUnindexedTablesAndDeaccess) {
  // CPLCleanupTLS runs CSVFreeTLS, which deaccesses every table still cached
  // in thread-local storage, and also destroys the per-thread error context.
  // Declare it first so that it runs last: after the quiet error handler has
  // been popped and after the vsimem wrappers have been destroyed, so nothing
  // re-allocates TLS state afterwards.
  absl::Cleanup cleanup = [] {
    SetCSVFilenameHook(nullptr);
    CPLCleanupTLS();
  };
  WithQuietHandler quiet;

  const char kIndexedFile[] = "/vsimem/csv_indexed_table.csv";
  const char kIndexedData[] =
      "id,name,val\n"
      "# comment row ignored by ingest\n"
      "10,\"Ten\nMultiline\",A\n"
      "20,TwentyFirst,B1\n"
      "20,TwentySecond,B2\n"
      "30,Thirty,C\n";
  VsiMemTempWrapper wrap1(kIndexedFile, kIndexedData);

  const char kUnindexedFile[] = "/vsimem/csv_unindexed_table.csv";
  const char kUnindexedData[] =
      "id,name,val\n"
      "30,Thirty,300\n"
      "short_row\n"
      "10,Ten,100\n"
      "20,Twenty,200\n";
  VsiMemTempWrapper wrap2(kUnindexedFile, kUnindexedData);

  // Indexed binary search: duplicate key, lower bound, upper bound, misses
  char** rec20 = CSVScanFileByName(kIndexedFile, "id", "20", CC_Integer);
  ASSERT_NE(nullptr, rec20);
  EXPECT_STREQ("TwentyFirst", rec20[1]);

  char** rec10 = CSVScanFileByName(kIndexedFile, "id", "10", CC_Integer);
  ASSERT_NE(nullptr, rec10);
  EXPECT_STREQ("A", rec10[2]);

  char** rec30 = CSVScanFileByName(kIndexedFile, "id", "30", CC_Integer);
  ASSERT_NE(nullptr, rec30);
  EXPECT_STREQ("Thirty", rec30[1]);

  EXPECT_EQ(nullptr, CSVScanFileByName(kIndexedFile, "id", "5", CC_Integer));
  EXPECT_EQ(nullptr, CSVScanFileByName(kIndexedFile, "id", "99", CC_Integer));
  EXPECT_EQ(nullptr,
            CSVScanFileByName(kIndexedFile, "no_such_col", "10", CC_Integer));
  EXPECT_EQ(nullptr, CSVScanFile(kIndexedFile, -1, "10", CC_Integer));
  EXPECT_EQ(nullptr,
            CSVScanFile("/vsimem/missing_csv.csv", 0, "10", CC_Integer));
  EXPECT_EQ(-1, CSVGetFileFieldId("/vsimem/missing_csv.csv", "id"));

  // Unindexed table: CSVGetField, cached current record hit, CC_ApproxString
  EXPECT_STREQ("Ten",
               CSVGetField(kUnindexedFile, "id", "10", CC_Integer, "name"));
  // Second call on same key uses cached record because bNonUniqueKey is false
  EXPECT_STREQ("100",
               CSVGetField(kUnindexedFile, "id", "10", CC_Integer, "val"));
  EXPECT_STREQ("300", CSVGetField(kUnindexedFile, "name", "thirty",
                                  CC_ApproxString, "val"));
  EXPECT_STREQ("", CSVGetField("/vsimem/missing_csv.csv", "id", "10",
                               CC_Integer, "val"));
  EXPECT_STREQ("",
               CSVGetField(kUnindexedFile, "bad_key", "10", CC_Integer, "val"));
  EXPECT_STREQ(
      "", CSVGetField(kUnindexedFile, "id", "10", CC_Integer, "bad_target"));

  // CSVRewind and CSVGetNextLine
  CSVRewind(kUnindexedFile);
  int count = 0;
  while (CSVGetNextLine(kUnindexedFile) != nullptr) {
    count++;
  }
  EXPECT_EQ(4, count);
  EXPECT_EQ(nullptr, CSVGetNextLine("/vsimem/missing_csv.csv"));

  // GDALDefaultCSVFilename matches already-accessed table by basename
  EXPECT_STREQ(kIndexedFile, GDALDefaultCSVFilename("csv_indexed_table.csv"));

  // CSVFilename and SetCSVFilenameHook
  EXPECT_STREQ(kIndexedFile, CSVFilename("csv_indexed_table.csv"));
  SetCSVFilenameHook(CustomCSVHook);
  EXPECT_STREQ("/custom_hook/my.csv", CSVFilename("my.csv"));
  SetCSVFilenameHook(nullptr);

  // Deaccess non-matching, then middle/tail table, then all
  CSVDeaccess("/vsimem/not_in_list.csv");
  CSVDeaccess(kIndexedFile);
  CSVDeaccess(nullptr);

  // Access once more so that CSVFreeTLS (invoked from CPLCleanupTLS in
  // `cleanup`) has a non-empty table list to free.
  EXPECT_EQ(0, CSVGetFileFieldId(kIndexedFile, "id"));
}

}  // namespace
}  // namespace autotest2
