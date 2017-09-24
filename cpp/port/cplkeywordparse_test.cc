// Tests for GDAL's keyword parser.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cplkeywordparser.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#undef DEBUG  // TODO(schwehr): Why does this test fail with debug defined?

#include "port/cplkeywordparser.h"

#include <string>

#include "gunit.h"
#include "port/cpl_vsi.h"

// Tests parsing the data from gdrivers/data/testtil.til.
TEST(CPLKeywordParser, BasicData) {
  const char filename[] = "/vsimem/cplkeywordparser_test.txt";

  VSILFILE *file = VSIFOpenL(filename, "wb");
  string data(
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

  char **all = parser.GetAllKeywords();
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

  VSILFILE *file = VSIFOpenL(filename, "wb");
  string data(
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

  char **all = parser.GetAllKeywords();
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

  VSILFILE *file = VSIFOpenL(filename, "wb");
  string data("junk\n");
  VSIFWriteL(data.c_str(), data.length(), 1, file);
  VSIFCloseL(file);
  file = VSIFOpenL(filename, "rb");

  CPLKeywordParser parser;
  int result = parser.Ingest(file);
  ASSERT_EQ(0, result);

  VSIFCloseL(file);
  VSIUnlink(filename);
}
