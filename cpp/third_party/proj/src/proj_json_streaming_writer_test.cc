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
// Unit tests for CPLJSonStreamingWriter JSON serialization, indentation,
// numeric formatting, and string escaping
// (proj/src/proj_json_streaming_writer.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/proj_json_streaming_writer.cpp

#include "third_party/proj4/proj/src/proj_json_streaming_writer.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"

namespace proj {
namespace {

using ::osgeo::proj::CPLJSonStreamingWriter;

void AppendJsonChunk(const char* text, void* user_data) {
  if (text != nullptr && user_data != nullptr) {
    *static_cast<std::string*>(user_data) += text;
  }
}

TEST(ProjJsonStreamingWriterTest, PrettyObjectAndNestedContainers) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetIndentationSize(2);
  writer.SetPrettyFormatting(true);

  {
    const auto root = writer.MakeObjectContext();
    writer.AddObjKey("name");
    writer.Add(std::string("PROJ"));
    writer.AddObjKey("empty_obj");
    {
      const auto empty_obj = writer.MakeObjectContext();
    }
    writer.AddObjKey("empty_arr");
    {
      const auto empty_arr =
          writer.MakeArrayContext(/*bForceSingleLine=*/false);
    }
    writer.AddObjKey("nested_arr");
    {
      const auto nested_arr =
          writer.MakeArrayContext(/*bForceSingleLine=*/false);
      writer.Add(1);
      writer.Add(2);
    }
  }

  constexpr char kExpected[] = R"json({
  "name": "PROJ",
  "empty_obj": {},
  "empty_arr": [],
  "nested_arr": [
    1,
    2
  ]
})json";
  EXPECT_EQ(kExpected, writer.GetString());
}

TEST(ProjJsonStreamingWriterTest, CompactFormattingAndCallbackSink) {
  std::string streamed;
  CPLJSonStreamingWriter writer(&AppendJsonChunk, &streamed);
  writer.SetPrettyFormatting(false);

  writer.StartObj();
  writer.AddObjKey("ok");
  writer.Add(true);
  writer.AddObjKey("no");
  writer.Add(false);
  writer.AddObjKey("missing");
  writer.AddNull();
  writer.AddObjKey("arr");
  writer.StartArray();
  writer.Add("text");
  writer.AddUnquoted("12345");
  writer.EndArray();
  writer.EndObj();

  EXPECT_EQ("", writer.GetString());
  EXPECT_EQ(
      R"json({"ok":true,"no":false,"missing":null,"arr":["text",12345]})json",
      streamed);
}

TEST(ProjJsonStreamingWriterTest, ForceSingleLineRestoresNewlineSetting) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetIndentationSize(4);
  EXPECT_TRUE(writer.GetNewLine());

  {
    const auto root = writer.MakeObjectContext();
    writer.AddObjKey("bbox");
    {
      const auto bbox = writer.MakeArrayContext(/*bForceSingleLine=*/true);
      EXPECT_FALSE(writer.GetNewLine());
      writer.Add(1);
      writer.Add(2);
      writer.Add(3);
    }
    EXPECT_TRUE(writer.GetNewLine());
    writer.AddObjKey("tail");
    writer.Add(true);
  }

  constexpr char kExpected[] = R"json({
    "bbox": [1, 2, 3],
    "tail": true
})json";
  EXPECT_EQ(kExpected, writer.GetString());

  // Verify SetNewline(false) applies across nested Pretty objects too.
  CPLJSonStreamingWriter single_line(nullptr, nullptr);
  single_line.SetNewline(false);
  EXPECT_FALSE(single_line.GetNewLine());
  single_line.StartObj();
  single_line.AddObjKey("a");
  single_line.Add(10);
  single_line.AddObjKey("b");
  single_line.Add(20);
  single_line.EndObj();
  EXPECT_EQ(R"json({"a": 10, "b": 20})json", single_line.GetString());
}

TEST(ProjJsonStreamingWriterTest, IntegerAndFloatingPointFormattingBranches) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetPrettyFormatting(false);

  writer.StartArray();
  writer.Add(static_cast<int>(-42));
  writer.Add(static_cast<unsigned int>(4000000000U));
  writer.Add(
      static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::min()));
  writer.Add(
      static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::max()));
  writer.Add(static_cast<std::uint64_t>(0ULL));
  writer.Add(
      static_cast<std::uint64_t>(std::numeric_limits<std::uint64_t>::max()));

  // float: NaN, +/-Inf, default and explicit precision.
  writer.Add(std::numeric_limits<float>::quiet_NaN());
  writer.Add(std::numeric_limits<float>::infinity());
  writer.Add(-std::numeric_limits<float>::infinity());
  writer.Add(1.5f);
  writer.Add(1.23456f, 3);

  // double: NaN, +/-Inf, integer fast-path [INT_MIN, INT_MAX], and %.ng path.
  writer.Add(std::numeric_limits<double>::quiet_NaN());
  writer.Add(std::numeric_limits<double>::infinity());
  writer.Add(-std::numeric_limits<double>::infinity());
  writer.Add(2005.0);
  writer.Add(static_cast<double>(std::numeric_limits<int>::min()));
  writer.Add(static_cast<double>(std::numeric_limits<int>::max()));
  writer.Add(3000000000.0);
  writer.Add(-3000000000.0);
  writer.Add(0.125);
  writer.Add(3.1415926, 4);
  writer.EndArray();

  constexpr char kExpected[] =
      "[-42,4000000000,-9223372036854775808,9223372036854775807,"
      "0,18446744073709551615,"
      "\"NaN\",\"Infinity\",\"-Infinity\",1.5,1.23,"
      "\"NaN\",\"Infinity\",\"-Infinity\",2005,-2147483648,2147483647,"
      "3000000000,-3000000000,0.125,3.142]";
  EXPECT_EQ(kExpected, writer.GetString());
}

TEST(ProjJsonStreamingWriterTest, EscapesControlCharactersQuotesAndUtf8) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetPrettyFormatting(false);

  std::string input = "a\"b\\c\bd\fe\nf\rg\th";
  input.push_back('\0');
  input.push_back('\x01');
  input.push_back('\x1f');
  input += " caf\xc3\xa9";

  writer.StartObj();
  writer.AddObjKey("k\"\\");
  writer.Add(input);
  writer.EndObj();

  EXPECT_EQ(R"json({"k\"\\":"a\"b\\c\bd\fe\nf\rg\th\u0000\u0001\u001F caf)json"
            "\xc3\xa9\"}",
            writer.GetString());
}

}  // namespace
}  // namespace proj
