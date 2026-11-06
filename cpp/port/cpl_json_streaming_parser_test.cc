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
// Tests for GDAL's JSON streaming parser (cpl_json_streaming_parser.cpp).

#include "third_party/gdal/port/cpl_json_streaming_parser.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

class RecordingParser : public CPLJSonStreamingParser {
 public:
  std::vector<std::string> events;
  std::vector<std::string> strings;
  std::vector<std::string> numbers;
  std::vector<std::string> keys;
  std::vector<std::string> exceptions;
  bool stop_on_first_string = false;

  void CallBaseMethods() {
    CPLJSonStreamingParser::String("s");
    CPLJSonStreamingParser::Number("1");
    CPLJSonStreamingParser::Boolean(true);
    CPLJSonStreamingParser::Null();
    CPLJSonStreamingParser::StartObject();
    CPLJSonStreamingParser::EndObject();
    CPLJSonStreamingParser::StartObjectMember("k");
    CPLJSonStreamingParser::StartArray();
    CPLJSonStreamingParser::EndArray();
    CPLJSonStreamingParser::StartArrayMember();
    CPLJSonStreamingParser::Exception("e");
  }

 protected:
  void String(std::string_view sValue) override {
    events.push_back("String:" + std::string(sValue));
    strings.emplace_back(sValue);
    if (stop_on_first_string) {
      StopParsing();
    }
  }
  void Number(std::string_view sValue) override {
    events.push_back("Number:" + std::string(sValue));
    numbers.emplace_back(sValue);
  }
  void Boolean(bool b) override {
    events.push_back(b ? "Boolean:true" : "Boolean:false");
  }
  void Null() override { events.push_back("Null"); }
  void StartObject() override { events.push_back("StartObject"); }
  void EndObject() override { events.push_back("EndObject"); }
  void StartObjectMember(std::string_view sKey) override {
    events.push_back("Key:" + std::string(sKey));
    keys.emplace_back(sKey);
  }
  void StartArray() override { events.push_back("StartArray"); }
  void EndArray() override { events.push_back("EndArray"); }
  void StartArrayMember() override { events.push_back("StartArrayMember"); }
  void Exception(const char* pszMessage) override {
    exceptions.emplace_back(pszMessage);
  }
};

bool ParseAllAtOnce(RecordingParser* parser, std::string_view json) {
  parser->Reset();
  parser->events.clear();
  parser->strings.clear();
  parser->numbers.clear();
  parser->keys.clear();
  parser->exceptions.clear();
  return parser->Parse(json, true);
}

bool ParseCharByChar(RecordingParser* parser, std::string_view json) {
  parser->Reset();
  parser->events.clear();
  parser->strings.clear();
  parser->numbers.clear();
  parser->keys.clear();
  parser->exceptions.clear();
  for (size_t i = 0; i < json.size(); ++i) {
    if (!parser->Parse(json.substr(i, 1), i + 1 == json.size())) {
      return false;
    }
  }
  if (json.empty()) {
    return parser->Parse("", true);
  }
  return true;
}

TEST(CplJsonStreamingParserTest, GetSerializedStringAndBaseNoOps) {
  RecordingParser p;
  p.CallBaseMethods();

  std::string s =
      CPLJSonStreamingParser::GetSerializedString("a\b\f\n\r\t\"\\\x02z");
  EXPECT_EQ(s, "\"a\\b\\f\\n\\r\\t\\\"\\\\\\u0002z\"");
}

TEST(CplJsonStreamingParserTest, ValidFullAndChunkedDocument) {
  const char* kJson =
      " \r\n\n\r{\r\n"
      "  \"str_fast\": \"simple\",\n"
      "  \"str\\nesc\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\",\n"
      "  \"arr_empty\": [],\n"
      "  \"obj_empty\": {},\n"
      "  \"arr\": [true, false, null, 0, -12.34e+5, 1E-2, Infinity, -Infinity, "
      "NaN, nan],\n"
      "  \"nested\": {\"a\": [1, 2]}\n"
      "} ";

  RecordingParser p1;
  EXPECT_TRUE(ParseAllAtOnce(&p1, kJson));
  EXPECT_FALSE(p1.ExceptionOccurred());
  EXPECT_THAT(p1.keys, ElementsAre("str_fast", "str\nesc", "arr_empty",
                                   "obj_empty", "arr", "nested", "a"));
  EXPECT_THAT(p1.strings, ElementsAre("simple", "\"\\/\b\f\n\r\t"));
  // Numbers are delivered verbatim, including the non-standard
  // Infinity/NaN spellings.
  EXPECT_THAT(p1.numbers, ElementsAre("0", "-12.34e+5", "1E-2", "Infinity",
                                      "-Infinity", "NaN", "nan", "1", "2"));
  EXPECT_THAT(
      p1.events,
      ElementsAre("StartObject", "Key:str_fast", "String:simple",
                  "Key:str\nesc", "String:\"\\/\b\f\n\r\t", "Key:arr_empty",
                  "StartArray", "EndArray", "Key:obj_empty", "StartObject",
                  "EndObject", "Key:arr", "StartArray", "StartArrayMember",
                  "Boolean:true", "StartArrayMember", "Boolean:false",
                  "StartArrayMember", "Null", "StartArrayMember", "Number:0",
                  "StartArrayMember", "Number:-12.34e+5", "StartArrayMember",
                  "Number:1E-2", "StartArrayMember", "Number:Infinity",
                  "StartArrayMember", "Number:-Infinity", "StartArrayMember",
                  "Number:NaN", "StartArrayMember", "Number:nan", "EndArray",
                  "Key:nested", "StartObject", "Key:a", "StartArray",
                  "StartArrayMember", "Number:1", "StartArrayMember",
                  "Number:2", "EndArray", "EndObject", "EndObject"));

  // Feeding the document one byte at a time must yield the same events.
  RecordingParser p2;
  EXPECT_TRUE(ParseCharByChar(&p2, kJson));
  EXPECT_FALSE(p2.ExceptionOccurred());
  EXPECT_EQ(p1.events, p2.events);
}

TEST(CplJsonStreamingParserTest, TopLevelScalarsAndStopParsing) {
  RecordingParser p;
  EXPECT_TRUE(ParseAllAtOnce(&p, "true"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "false"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "null"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "42.5"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "Infinity"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "-Infinity"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "NaN"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "nan"));
  EXPECT_TRUE(ParseAllAtOnce(&p, "\"top_str\""));
  EXPECT_TRUE(ParseCharByChar(&p, "true"));
  EXPECT_TRUE(ParseCharByChar(&p, "false"));
  EXPECT_TRUE(ParseCharByChar(&p, "null"));
  EXPECT_TRUE(ParseCharByChar(&p, "42.5"));
  EXPECT_TRUE(ParseCharByChar(&p, "   "));

  p.stop_on_first_string = true;
  EXPECT_FALSE(ParseAllAtOnce(&p, "[\"stop_here\", \"unreached\"]"));
  EXPECT_FALSE(p.ExceptionOccurred());
  // StopParsing() must prevent any further callbacks.
  EXPECT_THAT(p.strings, ElementsAre("stop_here"));
}

TEST(CplJsonStreamingParserTest, UnicodeEscapesAndSurrogates) {
  RecordingParser p;
  // 1) 1-byte (\u0041 = 'A'), 2-byte (\u00e9, \u07FF), 3-byte (\u20AC),
  //    4-byte surrogate pair (\uD834\uDD1E = U+1D11E).
  const char* kValidUnicode =
      "[\"\\u0041\", \"\\u00e9\", \"\\u07FF\", \"\\u20ac\", "
      "\"\\uD834\\uDD1E\"]";
  EXPECT_TRUE(ParseAllAtOnce(&p, kValidUnicode));
  EXPECT_EQ(p.strings.size(), 5u);
  EXPECT_EQ(p.strings[0], "A");
  EXPECT_EQ(p.strings[1], "\xC3\xA9");
  EXPECT_EQ(p.strings[2], "\xDF\xBF");
  EXPECT_EQ(p.strings[3], "\xE2\x82\xAC");
  EXPECT_EQ(p.strings[4], "\xF0\x9D\x84\x9E");

  // Also test char-by-char so chunk boundary hits inside unicode escapes.
  EXPECT_TRUE(ParseCharByChar(&p, kValidUnicode));
  EXPECT_EQ(p.strings[4], "\xF0\x9D\x84\x9E");

  // 2) Malformed surrogate combinations (replaced with U+FFFD \xEF\xBF\xBD):
  // - Lone low surrogate: \uDC00
  // - High surrogate followed by non-low surrogate \u0041
  // - High surrogate followed by non-u escape: \uD800\n
  // - High surrogate followed by normal char: \uD800X
  // - High surrogate at end of string: \uD800
  const char* kMalformedSurrogates =
      "[\"\\uDC00\", \"\\uD800\\u0041\", \"\\uD800\\n\", \"\\uD800X\", "
      "\"\\uD800\"]";
  EXPECT_TRUE(ParseAllAtOnce(&p, kMalformedSurrogates));
  ASSERT_EQ(p.strings.size(), 5u);
  for (const auto& s : p.strings) {
    EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos);
  }

  // 3) Illegal hex character in first 4 hex digits and in second 4 hex digits.
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"\\u123G\""));
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"\\uD800\\u123G\""));
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"\\uD800\\u\""));
}

TEST(CplJsonStreamingParserTest, LimitsAndSyntaxErrors) {
  RecordingParser p;

  // Max depth exceeded for objects and arrays (including at top-level INIT
  // state when MaxDepth == 1, where m_aState.size() == 1).
  p.SetMaxDepth(1);
  EXPECT_FALSE(ParseAllAtOnce(&p, "["));
  EXPECT_THAT(p.exceptions,
              ElementsAre(HasSubstr("Too many nested objects and/or arrays")));
  EXPECT_FALSE(ParseAllAtOnce(&p, "{\"key\":{}}"));
  EXPECT_THAT(p.exceptions,
              ElementsAre(HasSubstr("Too many nested objects and/or arrays")));
  p.SetMaxDepth(2);
  EXPECT_FALSE(ParseAllAtOnce(&p, "[[[]]]"));
  EXPECT_THAT(p.exceptions,
              ElementsAre(HasSubstr("Too many nested objects and/or arrays")));
  EXPECT_FALSE(ParseAllAtOnce(&p, "{\"a\":{\"b\":{\"c\":1}}}"));
  EXPECT_THAT(p.exceptions,
              ElementsAre(HasSubstr("Too many nested objects and/or arrays")));
  p.SetMaxDepth(1024);

  // Unterminated array with trailing whitespace in final chunk.
  p.Reset();
  p.exceptions.clear();
  EXPECT_TRUE(p.Parse("[1,", false));
  EXPECT_FALSE(p.Parse("   ", true));
  EXPECT_TRUE(p.ExceptionOccurred());
  EXPECT_THAT(p.exceptions, ElementsAre(HasSubstr("Unterminated array")));

  // Max string size exceeded in fast path and slow (escaped/chunked) path.
  p.SetMaxStringSize(4);
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"12345\""));
  EXPECT_THAT(p.exceptions, ElementsAre(HasSubstr("Too many characters")));
  EXPECT_FALSE(ParseCharByChar(&p, "\"12345\""));
  EXPECT_THAT(p.exceptions, ElementsAre(HasSubstr("Too many characters")));
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"12\\n45\""));
  EXPECT_THAT(p.exceptions, ElementsAre(HasSubstr("Too many characters")));
  p.SetMaxStringSize(10000000);

  // Number > 1024 chars.
  std::string huge_num(1030, '9');
  EXPECT_FALSE(ParseAllAtOnce(&p, huge_num));
  EXPECT_THAT(p.exceptions,
              ElementsAre(HasSubstr("Too many characters in number")));

  // Various invalid JSON inputs covering every error branch and exception text.
  struct InvalidCase {
    std::string_view input;
    std::string_view expected_exception;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.input = "true false", .expected_exception = "Unexpected character (f)"},
      {.input = "@", .expected_exception = "Unexpected character (@)"},
      {.input = "x", .expected_exception = "Unexpected character (x)"},
      {.input = "}", .expected_exception = "Unexpected character (})"},
      {.input = "]", .expected_exception = "Unexpected character (])"},
      {.input = "\"unterminated", .expected_exception = "Unterminated string"},
      {.input = "\"", .expected_exception = "Unterminated string"},
      {.input = "\"\\", .expected_exception = "Unterminated string"},
      {.input = "\"\\\"", .expected_exception = "Unterminated string"},
      {.input = "\"\\u", .expected_exception = "Unterminated string"},
      {.input = "\"\\u12\"",
       .expected_exception = "Illegal character in unicode sequence"},
      {.input = "\"\\u000", .expected_exception = "Unterminated string"},
      {.input = "\"\\x\"",
       .expected_exception = "Illegal escape sequence (\\x)"},
      {.input = "\"\\x", .expected_exception = "Illegal escape sequence (\\x)"},
      {.input = "\"\\ux",
       .expected_exception = "Illegal character in unicode sequence (\\x)"},
      {.input = "\"\\uD834\\ux",
       .expected_exception = "Illegal character in unicode sequence (\\x)"},
      {.input = "123a", .expected_exception = "Unexpected character (a)"},
      {.input = "1x", .expected_exception = "Unexpected character (x)"},
      {.input = "Inf", .expected_exception = "Invalid number"},
      {.input = "Inf,", .expected_exception = "Unexpected character (,)"},
      {.input = "-Inf", .expected_exception = "Invalid number"},
      {.input = "-Inf,", .expected_exception = "Unexpected character (,)"},
      {.input = "infinit", .expected_exception = "Invalid number"},
      {.input = "infinityx", .expected_exception = "Unexpected character (x)"},
      {.input = "Na", .expected_exception = "Invalid number"},
      {.input = "Na,", .expected_exception = "Unexpected character (,)"},
      {.input = "na", .expected_exception = "Invalid number"},
      {.input = "nanx", .expected_exception = "Unexpected character (x)"},
      {.input = "[", .expected_exception = "Unterminated array"},
      {.input = "[1", .expected_exception = "Unterminated array"},
      {.input = "[,", .expected_exception = "Unexpected character (,)"},
      {.input = "[|", .expected_exception = "Unexpected character (|)"},
      {.input = "[,]", .expected_exception = "Unexpected character (,)"},
      {.input = "[1,,2]", .expected_exception = "Unexpected character (,)"},
      {.input = "[true,,true]",
       .expected_exception = "Unexpected character (,)"},
      {.input = "[1,]", .expected_exception = "Missing value"},
      {.input = "[true,]", .expected_exception = "Missing value"},
      {.input = "[1 2]", .expected_exception = "Unexpected state"},
      {.input = "[true true]", .expected_exception = "Unexpected state"},
      {.input = "[@]", .expected_exception = "Unexpected character (@)"},
      {.input = "{", .expected_exception = "Unterminated object"},
      {.input = "{\"a\":1", .expected_exception = "Unterminated object"},
      {.input = "{\"x\"", .expected_exception = "Unterminated object"},
      {.input = "{\"x\":", .expected_exception = "Unterminated object"},
      {.input = "{,}", .expected_exception = "Unexpected character (,)"},
      {.input = "{ ,", .expected_exception = "Unexpected character (,)"},
      {.input = "{\"x\",", .expected_exception = "Unexpected character (,)"},
      {.input = "{\"a\":1,,}",
       .expected_exception = "Unexpected character (,)"},
      {.input = "{:}", .expected_exception = "Unexpected character (:)"},
      {.input = "{ :", .expected_exception = "Unexpected character (:)"},
      {.input = "{ |", .expected_exception = "Unexpected character (|)"},
      {.input = "{ 1", .expected_exception = "Unexpected character (1)"},
      {.input = "{\"a\"::1}", .expected_exception = "Unexpected character (:)"},
      {.input = "{\"a\"}", .expected_exception = "Missing value"},
      {.input = "{\"x\"}", .expected_exception = "Missing value"},
      {.input = "{\"a\":}", .expected_exception = "Missing value"},
      {.input = "{1:2}", .expected_exception = "Unexpected character (1)"},
      {.input = "{\"a\" x}", .expected_exception = "Unexpected character (x)"},
      {.input = "{\"a\":1 2}", .expected_exception = "Unexpected state"},
      {.input = "{\"x\": 1 2", .expected_exception = "Unexpected state"},
      {.input = "{\"a\":1 @}",
       .expected_exception = "Unexpected character (@)"},
      {.input = "tru", .expected_exception = "Unexpected character"},
      {.input = "tru1", .expected_exception = "Unexpected character (1)"},
      {.input = "truX", .expected_exception = "Unexpected character (X)"},
      {.input = "truxe", .expected_exception = "Unexpected character (x)"},
      {.input = "trueX", .expected_exception = "Unexpected character (X)"},
      {.input = "truex", .expected_exception = "Unexpected character (x)"},
      {.input = "tr@", .expected_exception = "Unexpected character (@)"},
      {.input = "fals", .expected_exception = "Unexpected character"},
      {.input = "falsX", .expected_exception = "Unexpected character (X)"},
      {.input = "falsxe", .expected_exception = "Unexpected character (x)"},
      {.input = "falseX", .expected_exception = "Unexpected character (X)"},
      {.input = "falsex", .expected_exception = "Unexpected character (x)"},
      {.input = "fa@", .expected_exception = "Unexpected character (@)"},
      {.input = "nul", .expected_exception = "Unexpected character"},
      {.input = "nulX", .expected_exception = "Unexpected character (X)"},
      {.input = "nulxl", .expected_exception = "Unexpected character (x)"},
      {.input = "nullX", .expected_exception = "Unexpected character (X)"},
      {.input = "nullx", .expected_exception = "Unexpected character (x)"},
      {.input = "nu@", .expected_exception = "Unexpected character (@)"},
  };

  for (const auto& tc : kInvalidCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_FALSE(ParseAllAtOnce(&p, tc.input));
    EXPECT_TRUE(p.ExceptionOccurred());
    EXPECT_THAT(p.exceptions, ElementsAre(HasSubstr(tc.expected_exception)));
  }

  // Exercise the chunked (m_osToken non-empty) delimiter-break path in NUMBER
  // state on incomplete Infinity/-Infinity/NaN tokens.
  for (std::string_view input : {"Inf,", "-Inf,", "Na,"}) {
    SCOPED_TRACE(input);
    EXPECT_FALSE(ParseCharByChar(&p, input));
    EXPECT_TRUE(p.ExceptionOccurred());
    EXPECT_THAT(p.exceptions, ElementsAre(HasSubstr("Invalid number")));
  }
}

}  // namespace
}  // namespace autotest2
