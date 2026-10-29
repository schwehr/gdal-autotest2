// Tests for GDAL's keyword parser.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cplkeywordparser.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cplkeywordparser.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_vsi.h"

// Tests parsing the data from gdrivers/data/testtil.til.
TEST(CPLKeywordParser, BasicData) {
  const char filename[] = "/vsimem/cplkeywordparser_test.txt";

  VSILFILE* file = VSIFOpenL(filename, "wb");
  std::string data(
      "numTiles = 1\n"
      "TILE_1.filename = \"byte.tif\"\n"
      "TILE_1.ULColOffset = 0\n"
      "TILE_1.ULRowOffset = 0\n"
      "TILE_1.LRColOffset = 20\n"
      "TILE_1.LRRowOffset = 20\n"
      "END;\n");
  VSIFWriteL(data.c_str(), data.length(), 1, file);
  VSIFCloseL(file);
  file = VSIFOpenL(filename, "rb");

  CPLKeywordParser parser;
  int result = parser.Ingest(file);
  ASSERT_EQ(1, result);

  VSIFCloseL(file);
  VSIUnlink(filename);

  char** all = parser.GetAllKeywords();
  ASSERT_STREQ("numTiles=1", all[0]);
  ASSERT_STREQ("TILE_1.filename=\"byte.tif\"", all[1]);
  ASSERT_STREQ("TILE_1.ULColOffset=0", all[2]);
  ASSERT_STREQ("TILE_1.ULRowOffset=0", all[3]);
  ASSERT_STREQ("TILE_1.LRColOffset=20", all[4]);
  ASSERT_STREQ("TILE_1.LRRowOffset=20", all[5]);
  ASSERT_STREQ(nullptr, all[6]);

  ASSERT_STREQ("1", parser.GetKeyword("numTiles"));
  ASSERT_STREQ("\"byte.tif\"", parser.GetKeyword("TILE_1.filename"));
  ASSERT_STREQ("0", parser.GetKeyword("TILE_1.ULColOffset"));
  ASSERT_STREQ("0", parser.GetKeyword("TILE_1.ULRowOffset"));
  ASSERT_STREQ("20", parser.GetKeyword("TILE_1.LRColOffset"));
  ASSERT_STREQ("20", parser.GetKeyword("TILE_1.LRRowOffset"));

  ASSERT_STREQ(nullptr, parser.GetKeyword("DoesNotExist"));
  ASSERT_STREQ(nullptr, parser.GetKeyword("DoesNotExist", nullptr));
  ASSERT_STREQ("a", parser.GetKeyword("DoesNotExist", "a"));
}

// Tests parsing with a group defined.
TEST(CPLKeywordParser, DataWithGroups) {
  const char filename[] = "/vsimem/cplkeywordparser_test2.txt";

  VSILFILE* file = VSIFOpenL(filename, "wb");
  std::string data(
      "version = \"AA\";\n"
      "generationTime = 2014-07-09T10:11:12.001Z;\n"
      "BEGIN_GROUP = BAND_P\n"
      " ULLon = 12.23;\n"
      " ULLat = 45.678;\n"
      " SomeFactor = -2.3e-99;\n"
      "END_GROUP = BAND_P\n"
      "between = 999;\n"
      "BEGIN_GROUP = IMAGE_1\n"
      " CatId = \"abc123\";\n"
      " SomeList = (\n"
      " (1, 2.340),\n"
      " (5, 6.2) );\n"
      "END_GROUP = IMAGE_1\n"
      "END;\n");
  VSIFWriteL(data.c_str(), data.length(), 1, file);
  VSIFCloseL(file);
  file = VSIFOpenL(filename, "rb");

  CPLKeywordParser parser;
  int result = parser.Ingest(file);
  ASSERT_EQ(1, result);

  VSIFCloseL(file);
  VSIUnlink(filename);

  char** all = parser.GetAllKeywords();
  ASSERT_STREQ("version=\"AA\"", all[0]);
  ASSERT_STREQ("generationTime=2014-07-09T10:11:12.001Z", all[1]);
  ASSERT_STREQ("BAND_P.ULLon=12.23", all[2]);
  ASSERT_STREQ(nullptr, all[8]);

  ASSERT_STREQ("\"AA\"", parser.GetKeyword("version"));
  ASSERT_STREQ("12.23", parser.GetKeyword("BAND_P.ULLon"));
  ASSERT_STREQ("999", parser.GetKeyword("between"));
  ASSERT_STREQ("((1,2.340),(5,6.2))", parser.GetKeyword("IMAGE_1.SomeList"));

  ASSERT_STREQ(nullptr, parser.GetKeyword("IMAGE_1.DoesNotExist"));
}

// Tests parsing bad file.
TEST(CPLKeywordParser, BadData) {
  const char filename[] = "/vsimem/cplkeywordparser_test3.txt";

  VSILFILE* file = VSIFOpenL(filename, "wb");
  std::string data("junk\n");
  VSIFWriteL(data.c_str(), data.length(), 1, file);
  VSIFCloseL(file);
  file = VSIFOpenL(filename, "rb");

  CPLKeywordParser parser;
  int result = parser.Ingest(file);
  ASSERT_EQ(0, result);

  VSIFCloseL(file);
  VSIUnlink(filename);
}

TEST(CPLKeywordParser, AdvancedFeaturesAndEdgeCases) {
  const char filename[] = "/vsimem/cplkeywordparser_advanced.txt";

  // Include:
  // - > 1024 bytes before \nEND;\n so Ingest() needs a third 512-byte read
  //   and the (osHeaderText.size() > 520) end-of-header check executes
  // - C-style /* ... */ and # comments
  // - ISIS3 GROUP ... End_Group and End_Object without '='
  // - Values with quotes inside lists: ("(a)", 'b')
  // - Unquoted value with spaces ending with ; and \r\n
  // - Value with units <PIXEL/DEGREE>
  std::string padding(1100, ' ');
  std::string data =
      "/* C style comment */\n"
      "# Hash style comment\n" +
      padding +
      "\n"
      "GROUP = ISIS_GROUP\n"
      "  SingleQuoted = 'val_single';\n"
      "  ListWithQuotes = (\"(in_quotes)\", 'single_q');\n"
      "  UnquotedSpaces = value with spaces   ;\r\n"
      "  WithUnits = 4.0 <PIXEL / DEGREE>;\n"
      "End_Group\n"
      "BEGIN_GROUP = OBJ_GROUP\n"
      "  KeyObj = 123\n"
      "End_Object\n"
      "END;\n"
      "IgnoredTrailing = 1\n";

  VSILFILE* file = VSIFOpenL(filename, "wb");
  ASSERT_NE(nullptr, file);
  VSIFWriteL(data.c_str(), data.size(), 1, file);
  VSIFCloseL(file);

  file = VSIFOpenL(filename, "rb");
  ASSERT_NE(nullptr, file);
  CPLKeywordParser parser;
  EXPECT_EQ(1, parser.Ingest(file));
  VSIFCloseL(file);
  VSIUnlink(filename);

  EXPECT_STREQ("'val_single'", parser.GetKeyword("ISIS_GROUP.SingleQuoted"));
  EXPECT_STREQ("(\"(in_quotes)\",'single_q')",
               parser.GetKeyword("ISIS_GROUP.ListWithQuotes"));
  EXPECT_STREQ("value with spaces",
               parser.GetKeyword("ISIS_GROUP.UnquotedSpaces"));
  EXPECT_STREQ("4.0 <PIXEL/DEGREE>", parser.GetKeyword("ISIS_GROUP.WithUnits"));
  EXPECT_STREQ("123", parser.GetKeyword("OBJ_GROUP.KeyObj"));
}

TEST(CPLKeywordParser, ErrorConditions) {
  // Returns the Ingest() result, or -1 if the /vsimem/ file could not be
  // created/opened.
  auto ingest_str = [](const std::string& content) -> int {
    const char fname[] = "/vsimem/cplkeywordparser_err.txt";
    VSILFILE* fp = VSIFOpenL(fname, "wb");
    EXPECT_NE(nullptr, fp);
    if (fp == nullptr) return -1;
    VSIFWriteL(content.c_str(), content.size(), 1, fp);
    VSIFCloseL(fp);
    fp = VSIFOpenL(fname, "rb");
    EXPECT_NE(nullptr, fp);
    if (fp == nullptr) return -1;
    CPLKeywordParser parser;
    int res = parser.Ingest(fp);
    VSIFCloseL(fp);
    VSIUnlink(fname);
    return res;
  };

  // Unterminated C comment.
  EXPECT_EQ(0, ingest_str("/* unterminated comment"));

  // Unterminated double quote in word.
  EXPECT_EQ(0, ingest_str("key = \"unterminated"));

  // Unterminated single quote in word.
  EXPECT_EQ(0, ingest_str("key = 'unterminated"));

  // Missing value after '='.
  EXPECT_EQ(0, ingest_str("key = "));

  // Recursion limit: ReadGroup() refuses to descend to nesting level 100.
  // Both documents below are well-formed (every group is closed and the
  // header ends with END;), so the only difference is the nesting depth.
  auto nested_groups = [](int levels) {
    std::string doc;
    for (int i = 0; i < levels; ++i) doc += "BEGIN_GROUP = G\n";
    for (int i = 0; i < levels; ++i) doc += "END_GROUP = G\n";
    doc += "END;\n";
    return doc;
  };
  EXPECT_EQ(1, ingest_str(nested_groups(99)));
  EXPECT_EQ(0, ingest_str(nested_groups(100)));
}
