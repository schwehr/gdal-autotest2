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
#include "port/gdal_csv.h"

#include "gmock.h"
#include "gunit.h"

using testing::StartsWith;
using testing::EndsWith;

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

// TODO(schwehr): Test CSVReadParseLine.
// TODO(schwehr): Test CSVReadParseLine2.
// TODO(schwehr): Test CSVReadParseLineL.
// TODO(schwehr): Test CSVReadParseLine2L.
// TODO(schwehr): Test CSVScanLines.
// TODO(schwehr): Test CSVScanLinesL.
// TODO(schwehr): Test CSVScanLinesIndexed.
// TODO(schwehr): Test CSVScanLinesIngested.
// TODO(schwehr): Test CSVGetNextLine.
// TODO(schwehr): Test CSVScanFile.
// TODO(schwehr): Test CSVGetFieldId.
// TODO(schwehr): Test CSVGetFieldIdL.
// TODO(schwehr): Test CSVGetFileFieldId.
// TODO(schwehr): Test CSVScanFileByName.
// TODO(schwehr): Test CSVGetField.

TEST(CplCsvTest, GDALDefaultCSVFilename) {
  // Files that do not exist.
  // nullptr not allowed as an arg.
  EXPECT_THAT(GDALDefaultCSVFilename(""), StartsWith("/"));
  EXPECT_THAT(GDALDefaultCSVFilename(""), EndsWith("/"));
  EXPECT_STREQ("does_not_exist", GDALDefaultCSVFilename("does_not_exist"));
  EXPECT_STREQ("/foo/bar.csv", GDALDefaultCSVFilename("/foo/bar.csv"));
  EXPECT_STREQ("../baz.csv", GDALDefaultCSVFilename("../baz.csv"));

  // Try a file that GDAL does have.
  EXPECT_THAT(GDALDefaultCSVFilename("gcs.csv"), StartsWith("/"));
  EXPECT_THAT(GDALDefaultCSVFilename("gcs.csv"), EndsWith("/gcs.csv"));

  // GDAL specifically searches for these two files that can cause trouble.
  EXPECT_STREQ("datum.csv", GDALDefaultCSVFilename("datum.csv"));
  EXPECT_THAT(GDALDefaultCSVFilename("gdal_datum.csv"), StartsWith("/"));
  EXPECT_THAT(GDALDefaultCSVFilename("gdal_datum.csv"),
              EndsWith("/gdal_datum.csv"));

  // Test in memory filesystem.
  const string filename("/vsimem/gdaldefaultcsvfilename.txt");

  // Does not exist.
  EXPECT_STREQ(filename.c_str(), GDALDefaultCSVFilename(filename.c_str()));

  VSILFILE *file = VSIFOpenL(filename.c_str(), "wb");
  const string data("foo");
  VSIFWriteL(data.c_str(), data.length(), 1, file);
  VSIFCloseL(file);

  EXPECT_THAT(GDALDefaultCSVFilename(filename.c_str()), StartsWith("/"));
  EXPECT_THAT(GDALDefaultCSVFilename(filename.c_str()),
              EndsWith(filename.c_str()));

  VSIUnlink(filename.c_str());
}

// TODO(schwehr): Test CSVFilename.
// TODO(schwehr): Test SetCSVFilenameHook.

}  // namespace
}  // namespace autotest2
