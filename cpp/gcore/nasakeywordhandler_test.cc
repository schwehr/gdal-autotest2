// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/gcore/nasakeywordhandler.h"

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

TEST(NasaKeywordHandlerTest, ParseAndIngestFullFeatures) {
  const std::string pds_text =
      "/* PDS Header comment */ extra text on comment line\n"
      " # Hash comment line\n"
      "PDS_VERSION_ID = PDS3\n"
      "INT_VAL = 42\n"
      "FLOAT_VAL = 3.14\n"
      "QUOTED_STR = \"Hello\nWorld\r!\"\n"
      "SINGLE_QUOTED = 'SYMBOL_VAL'\n"
      "CONTINUED_WORD = PART1-\n"
      "  PART2\n"
      "MAP_RESOLUTION = 4.0 <PIXEL/DEGREE>\n"
      "INT_WITH_UNIT = 10 <METERS>\n"
      "STR_WITH_UNIT = FOO <BAR>\n"
      "ARRAY_WITH_UNIT = (1, 2.5, \"three\") <KM>\n"
      "SIMPLE_LIST = (Red, Green, #123456,\n"
      "               #654321)\n"
      "SET_LIST = {10, 20, 30}\n"
      "NESTED_LIST = ({1, 2}, {3, 4})\n"
      "OBJECT = IMAGE\n"
      "  Name = \"MAIN_IMG\"\n"
      "  LINES = 100\n"
      "END_OBJECT = IMAGE\n"
      "GROUP = PARAMS\n"
      "  A = 1\n"
      "END_GROUP\n"
      "GROUP = PARAMS\n"
      "  A = 2\n"
      "End_Group\n"
      "GROUP = PARAMS\n"
      "  A = 3\n"
      "End_Object\n" +
      std::string(600, ' ') + "\nEND\n";

  const char* mem_path = "/vsimem/nasa_test.lbl";
  VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(pds_text.data(), 1, pds_text.size(), fp);

  NASAKeywordHandler handler;
  handler.SetStripSurroundingQuotes(false);
  EXPECT_TRUE(handler.Ingest(fp, 0));

  EXPECT_STREQ(handler.GetKeyword("PDS_VERSION_ID", ""), "PDS3");
  EXPECT_STREQ(handler.GetKeyword("INT_VAL", ""), "42");
  EXPECT_STREQ(handler.GetKeyword("CONTINUED_WORD", ""), "PART1PART2");
  EXPECT_STREQ(handler.GetKeyword("IMAGE.LINES", ""), "100");
  EXPECT_STREQ(handler.GetKeyword("NON_EXISTENT", "DEF"), "DEF");
  EXPECT_NE(handler.GetKeywordList(), nullptr);

  CPLJSONObject json = handler.GetJsonObject();
  EXPECT_TRUE(json.IsValid());
  EXPECT_EQ(json.GetInteger("INT_VAL"), 42);
  EXPECT_TRUE(json["IMAGE_MAIN_IMG"].IsValid());
  EXPECT_TRUE(json["PARAMS"].IsValid());
  EXPECT_TRUE(json["PARAMS_2"].IsValid());
  EXPECT_TRUE(json["PARAMS_3"].IsValid());

  // Also test with StripSurroundingQuotes(true)
  NASAKeywordHandler handler_strip;
  handler_strip.SetStripSurroundingQuotes(true);
  EXPECT_TRUE(handler_strip.Parse(pds_text.c_str()));
  EXPECT_STREQ(handler_strip.GetKeyword("SINGLE_QUOTED", ""), "SYMBOL_VAL");

  VSIFCloseL(fp);
  VSIUnlink(mem_path);
}

TEST(NasaKeywordHandlerTest, ErrorCases) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // Unterminated double quote
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("KEY = \"unterminated"));
  }
  // Unterminated single quote
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("KEY = 'unterminated"));
  }
  // Missing '='
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("KEY_WITHOUT_EQUALS VAL\nEND\n"));
  }
  // Missing value after '='
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("KEY = \n"));
  }
  // Unpaired ')' in list
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("KEY = {1, 2)\nEND\n"));
  }
  // Unpaired '}' in list
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("KEY = (1, 2}\nEND\n"));
  }
  // Unterminated C comment
  {
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse("/* unterminated comment"));
  }
  // Too long group prefix (> 256 chars)
  {
    std::string long_group =
        "GROUP = " + std::string(260, 'A') + "\nA = 1\nEND_GROUP\nEND\n";
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse(long_group.c_str()));
  }
  // Negative offset in Ingest
  {
    const char* mem_path = "/vsimem/nasa_neg.lbl";
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Ingest(fp, -10));
    VSIFCloseL(fp);
    VSIUnlink(mem_path);
  }
}

}  // namespace
}  // namespace gdal
