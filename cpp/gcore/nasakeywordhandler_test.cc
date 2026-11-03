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
// Tests PDS/ISIS NASA keyword handler (gcore/nasakeywordhandler.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/nasakeywordhandler.cpp

#include "third_party/gdal/gcore/nasakeywordhandler.h"

#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/nasakeywordhandler_test";
constexpr char kMainPath[] = "/vsimem/nasakeywordhandler_test/nasa_test.lbl";
constexpr char kNegOffsetPath[] =
    "/vsimem/nasakeywordhandler_test/nasa_neg.lbl";

TEST(NasaKeywordHandlerTest, ParseAndIngestFullFeatures) {
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

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

  VsiMemTempWrapper temp_file(kMainPath, pds_text);
  VSILFILE* fp = VSIFOpenL(kMainPath, "rb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  NASAKeywordHandler handler;
  handler.SetStripSurroundingQuotes(false);
  EXPECT_TRUE(handler.Ingest(fp, 0));

  EXPECT_STREQ("PDS3", handler.GetKeyword("PDS_VERSION_ID", ""));
  EXPECT_STREQ("42", handler.GetKeyword("INT_VAL", ""));
  EXPECT_STREQ("PART1PART2", handler.GetKeyword("CONTINUED_WORD", ""));
  EXPECT_STREQ("100", handler.GetKeyword("IMAGE.LINES", ""));
  EXPECT_STREQ("DEF", handler.GetKeyword("NON_EXISTENT", "DEF"));
  EXPECT_NE(nullptr, handler.GetKeywordList());

  CPLJSONObject json = handler.GetJsonObject();
  EXPECT_TRUE(json.IsValid());
  EXPECT_EQ(42, json.GetInteger("INT_VAL"));
  EXPECT_TRUE(json["IMAGE_MAIN_IMG"].IsValid());
  EXPECT_TRUE(json["PARAMS"].IsValid());
  EXPECT_TRUE(json["PARAMS_2"].IsValid());
  EXPECT_TRUE(json["PARAMS_3"].IsValid());

  // Also test with StripSurroundingQuotes(true).
  NASAKeywordHandler handler_strip;
  handler_strip.SetStripSurroundingQuotes(true);
  EXPECT_TRUE(handler_strip.Parse(pds_text.c_str()));
  EXPECT_STREQ("SYMBOL_VAL", handler_strip.GetKeyword("SINGLE_QUOTED", ""));
}

TEST(NasaKeywordHandlerTest, ErrorCases) {
  constexpr std::string_view kInvalidInputs[] = {
      // Unterminated double quote
      "KEY = \"unterminated",
      // Unterminated single quote
      "KEY = 'unterminated",
      // Missing '='
      "KEY_WITHOUT_EQUALS VAL\nEND\n",
      // Missing value after '='
      "KEY = \n",
      // Unpaired ')' in list
      "KEY = {1, 2)\nEND\n",
      // Unpaired '}' in list
      "KEY = (1, 2}\nEND\n",
      // Unterminated C comment
      "/* unterminated comment",
  };
  for (std::string_view input : kInvalidInputs) {
    WithQuietHandler quiet;
    CPLErrorReset();
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse(std::string(input).c_str()));
    CPLErrorReset();
  }

  // Too long group prefix (> 256 chars) emits CPLError(CE_Failure).
  {
    const std::string long_group =
        "GROUP = " + std::string(260, 'A') + "\nA = 1\nEND_GROUP\nEND\n";
    WithQuietHandler quiet;
    CPLErrorReset();
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Parse(long_group.c_str()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Negative offset in Ingest.
  {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
    VsiMemTempWrapper temp_file(kNegOffsetPath, "PDS_VERSION_ID = PDS3\nEND\n");
    VSILFILE* fp = VSIFOpenL(kNegOffsetPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    WithQuietHandler quiet;
    CPLErrorReset();
    NASAKeywordHandler h;
    EXPECT_FALSE(h.Ingest(fp, -10));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
