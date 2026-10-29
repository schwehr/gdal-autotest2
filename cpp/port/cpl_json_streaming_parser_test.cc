// Copyright 2026 Google Inc. All Rights Reserved.
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
//
// Tests for GDAL's JSON streaming parser (cpl_json_streaming_parser.cpp).

#include "third_party/gdal/port/cpl_json_streaming_parser.h"

#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace gdal {
namespace {

using ::testing::ElementsAre;

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

  // Max depth exceeded for objects and arrays.
  p.SetMaxDepth(2);
  EXPECT_FALSE(ParseAllAtOnce(&p, "[[[]]]"));
  EXPECT_FALSE(ParseAllAtOnce(&p, "{\"a\":{\"b\":{\"c\":1}}}"));
  p.SetMaxDepth(1024);

  // Max string size exceeded in fast path and slow (escaped/chunked) path.
  p.SetMaxStringSize(4);
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"12345\""));
  EXPECT_FALSE(ParseCharByChar(&p, "\"12345\""));
  EXPECT_FALSE(ParseAllAtOnce(&p, "\"12\\n45\""));
  p.SetMaxStringSize(10000000);

  // Number > 1024 chars.
  std::string huge_num(1030, '9');
  EXPECT_FALSE(ParseAllAtOnce(&p, huge_num));

  // Various invalid JSON inputs covering every error branch.
  const char* kInvalidInputs[] = {
      "true false",  "@",           "\"unterminated",
      "\"\\x\"",     "\"\\",        "\"\\u12\"",
      "123a",        "Inf",         "Inf,",
      "-Inf",        "-Inf,",       "Na",
      "Na,",         "[1",          "{\"a\":1",
      "[,]",         "[1,,2]",      "[1,]",
      "[1 2]",       "[@]",         "{,}",
      "{\"a\":1,,}", "{:}",         "{\"a\"::1}",
      "{",           "{\"a\"}",     "{\"a\":}",
      "{1:2}",       "{\"a\":1 2}", "{\"a\":1 @}",
      "tru",         "truX",        "trueX",
      "tr@",         "fals",        "falsX",
      "falseX",      "fa@",         "nul",
      "nulX",        "nullX",       "nu@",
  };

  for (const char* bad : kInvalidInputs) {
    EXPECT_FALSE(ParseAllAtOnce(&p, bad)) << "Expected failure for: " << bad;
    EXPECT_TRUE(p.ExceptionOccurred()) << "Expected exception for: " << bad;
  }
}

}  // namespace
}  // namespace gdal
