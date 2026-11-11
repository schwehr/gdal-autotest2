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
// Unit tests for WKT parser error context formatting (proj/src/wkt_parser.cpp)
// and WKT1/WKT2 grammar error integration via proj_create_from_wkt.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/wkt_parser.cpp

#include "third_party/proj4/proj/src/wkt_parser.hpp"

#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

TEST(WktParserTest, ErrorAtStartAndMiddleOfShortSingleLineInput) {
  // 1. Error at offset 0 (n == 0, start_i == 0, 0 caret spaces).
  pj_wkt_parse_context ctx_start;
  constexpr char kShort[] = "BAD_TOKEN";
  ctx_start.pszInput = kShort;
  ctx_start.pszLastSuccess = kShort;
  pj_wkt_error(&ctx_start, "syntax error");
  EXPECT_EQ(
      "Parsing error : syntax error. Error occurred around:\n"
      "BAD_TOKEN\n"
      "^",
      ctx_start.errorMsg);

  // 2. Error in middle within 40 chars of start (0 < n <= 40, NUL before n+40).
  pj_wkt_parse_context ctx_mid;
  constexpr char kMid[] = "GEOGCS[\"WGS84\",DATUM[\"foo\"]]";
  ctx_mid.pszInput = kMid;
  ctx_mid.pszLastSuccess = kMid + 15;  // Points to 'D' of DATUM.
  pj_wkt_error(&ctx_mid, "unexpected keyword");
  EXPECT_EQ(
      "Parsing error : unexpected keyword. Error occurred around:\n"
      "GEOGCS[\"WGS84\",DATUM[\"foo\"]]\n"
      "               ^",
      ctx_mid.errorMsg);
}

TEST(WktParserTest, LongSingleLineInputTruncatesToFortyCharWindowAroundError) {
  // Construct 100-char string; error at index 50 crops to [10, 90) with 40
  // leading spaces before the caret.
  std::string input(100, 'A');
  input[50] = 'X';

  pj_wkt_parse_context ctx;
  ctx.pszInput = input.c_str();
  ctx.pszLastSuccess = input.c_str() + 50;
  pj_wkt_error(&ctx, "long line error");

  const std::string expected_window = input.substr(10, 80);
  const std::string expected =
      "Parsing error : long line error. Error occurred around:\n" +
      expected_window + "\n" + std::string(40, ' ') + "^";
  EXPECT_EQ(expected, ctx.errorMsg);
}

TEST(WktParserTest, MultilineInputSlicesCurrentLineAtLfAndCrLfBoundaries) {
  // 1. Newlines (\n and \r\n) both before (i <= n) and after (i > n) error.
  constexpr char kMulti[] = "LINE1\nLINE2\r\nLINE3_BAD\r\nLINE4";
  // "LINE1\n" (6) + "LINE2\r\n" (7) = 13 -> "LINE3_BAD" starts at 13, 'B'
  // at 19.
  pj_wkt_parse_context ctx;
  ctx.pszInput = kMulti;
  ctx.pszLastSuccess = kMulti + 19;
  pj_wkt_error(&ctx, "multiline error");
  EXPECT_EQ(
      "Parsing error : multiline error. Error occurred around:\n"
      "LINE3_BAD\n"
      "      ^",
      ctx.errorMsg);

  // 2. Standalone '\r' after error stops context snippet via i > n break.
  constexpr char kCrAfter[] = "ERR_HERE\rTAIL";
  pj_wkt_parse_context ctx_cr;
  ctx_cr.pszInput = kCrAfter;
  ctx_cr.pszLastSuccess = kCrAfter + 4;  // Points to 'H'.
  pj_wkt_error(&ctx_cr, "cr error");
  EXPECT_EQ(
      "Parsing error : cr error. Error occurred around:\n"
      "ERR_HERE\n"
      "    ^",
      ctx_cr.errorMsg);

  // 3. Error cursor landing directly on '\n' (i == n) resets ctxtMsg and sets
  // start_i = n + 1, advancing to the following line with 0 caret spaces.
  constexpr char kOnNewline[] = "FIRST\nSECOND";
  pj_wkt_parse_context ctx_at_nl;
  ctx_at_nl.pszInput = kOnNewline;
  ctx_at_nl.pszLastSuccess = kOnNewline + 5;  // Points to '\n'.
  pj_wkt_error(&ctx_at_nl, "at newline");
  EXPECT_EQ(
      "Parsing error : at newline. Error occurred around:\n"
      "SECOND\n"
      "^",
      ctx_at_nl.errorMsg);
}

TEST(WktParserTest, CreateFromWktProducesGrammarErrorWithCaretForWkt1AndWkt2) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  constexpr std::string_view kBadWkts[] = {
      // Malformed WKT1: valid tree structure with an extra unknown child node
      // so WKTParser::Private::build() succeeds and pj_wkt1_parse() runs under
      // STRICT=YES.
      "GEOGCS[\"WGS 84\","
      "DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],"
      "PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433],"
      "BAD_ELEMENT[\"foo\"]]",
      // Malformed WKT2: valid tree structure with an extra unknown child node
      // so WKTParser::Private::build() succeeds and pj_wkt2_parse() runs under
      // STRICT=YES.
      "GEOGCRS[\"WGS 84\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433],"
      "BAD_ELEMENT[\"foo\"]]",
  };
  const char* const kStrictOptions[] = {"STRICT=YES", nullptr};

  for (const std::string_view bad_wkt : kBadWkts) {
    const std::string wkt_str(bad_wkt);
    PROJ_STRING_LIST raw_warnings = nullptr;
    PROJ_STRING_LIST raw_errors = nullptr;
    ScopedPj pj(proj_create_from_wkt(ctx.get(), wkt_str.c_str(), kStrictOptions,
                                     &raw_warnings, &raw_errors));
    ScopedStringList warnings(raw_warnings);
    ScopedStringList errors(raw_errors);
    EXPECT_EQ(nullptr, pj.get()) << wkt_str;
    ASSERT_NE(nullptr, errors.get()) << wkt_str;
    ASSERT_NE(nullptr, errors.get()[0]) << wkt_str;
    EXPECT_THAT(errors.get()[0], HasSubstr("Parsing error :"));
    EXPECT_THAT(errors.get()[0], HasSubstr("^"));
  }
}

}  // namespace
}  // namespace proj
