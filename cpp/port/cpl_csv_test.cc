// Tests for GDAL's C String List (CSL) API.
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_csv.cpp
//
// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Both of these includes define functions in cpl_csv.cpp.
#include "port/cpl_csv.h"

#include <memory>
#include <string>
#include <vector>

#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/vsimem.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"
#include "port/gdal_csv.h"

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
    VSILFILE *file = VSIFOpenL(filename, "wb");
    VSIFCloseL(file);
  }

  VSILFILE *file = VSIFOpenL(filename, "rb");

  StringListPtr fields(CSVReadParseLine2L(file, ','), CSLDestroy);
  EXPECT_THAT(fields, IsNull());

  VSIFCloseL(file);
  VSIUnlink(filename);
}

TEST(CplCsvTest, CSVReadParseLine2LOneLine) {
  const char filename[] = "/vsimem/CSVReadParseLine2L.csv";

  VsiMemTempWrapper tmp_file(filename, "a,b,\n");

  VSILFILE *file = VSIFOpenL(filename, "rb");
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

  VSILFILE *file = VSIFOpenL(filename, "rb");
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

  VSILFILE *file = VSIFOpenL(filename, "rb");
  EXPECT_EQ(0, CSVGetFieldIdL(file, " "));
  EXPECT_EQ(1, CSVGetFieldIdL(file, ""));
  EXPECT_EQ(2, CSVGetFieldIdL(file, "abc "));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "abc"));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "a"));
  EXPECT_EQ(-1, CSVGetFieldIdL(file, "c "));
  VSIFCloseL(file);
}

// TODO(schwehr): Test CSVGetFieldIdL with invalid CSV.

TEST(CplCsvTest,  CSVGetFileFieldIdBasic) {
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

TEST(CplCsvTest,  CSVGetFileFieldIdMore) {
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

TEST(CplCsvTest,  CSVGetFileFieldIdEmptyAndSpace) {
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
  EXPECT_STREQ("", GDALDefaultCSVFilename(""));
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
  EXPECT_THAT(GDALDefaultCSVFilename(filename),
              EndsWith(filename));
}

// TODO(schwehr): Test CSVFilename.
// TODO(schwehr): Test SetCSVFilenameHook.

}  // namespace
}  // namespace autotest2
