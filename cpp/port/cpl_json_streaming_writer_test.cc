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
// Tests for GDAL's JSON streaming writer (cpl_json_streaming_writer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_json_streaming_writer.cpp

#include "third_party/gdal/port/cpl_json_streaming_writer.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_float.h"

namespace autotest2 {
namespace {

void AppendToStringCallback(const char* pszTxt, void* pUserData) {
  auto* out = static_cast<std::string*>(pUserData);
  *out += pszTxt;
}

TEST(CplJsonStreamingWriterTest, PrettyObjectAndArrayWithAllTypes) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetIndentationSize(2);
  EXPECT_TRUE(writer.GetNewLine());

  {
    auto obj = writer.MakeObjectContext();
    writer.AddObjKey("bool_true");
    writer.Add(true);
    writer.AddObjKey("bool_false");
    writer.Add(false);
    writer.AddObjKey("null_val");
    writer.AddNull();
    writer.AddObjKey("c_str");
    writer.Add("hello");
    writer.AddObjKey("std_str");
    writer.Add(std::string("world"));
    writer.AddObjKey("sv_escaped");
    writer.Add(std::string_view("quote\"bs\\b\bf\fn\nr\rt\tctrl\x01"));
    writer.AddObjKey("raw_json");
    writer.AddSerializedValue("{\"pre\":1}");
    writer.AddObjKey("int32");
    writer.Add(-42);
    writer.AddObjKey("uint32");
    writer.Add(42u);
    writer.AddObjKey("int64");
    writer.Add(static_cast<std::int64_t>(-9000000000LL));
    writer.AddObjKey("uint64");
    writer.Add(static_cast<std::uint64_t>(18000000000ULL));

    writer.AddObjKey("empty_obj");
    {
      auto empty_obj = writer.MakeObjectContext();
    }

    writer.AddObjKey("empty_arr");
    {
      auto empty_arr = writer.MakeArrayContext();
    }

    writer.AddObjKey("multiline_arr");
    {
      auto arr = writer.MakeArrayContext(false);
      writer.Add(1);
      writer.Add(2);
    }

    writer.AddObjKey("singleline_arr");
    {
      auto arr = writer.MakeArrayContext(true);
      writer.Add(10);
      writer.Add(20);
    }

    writer.AddObjKey("singleline_obj");
    writer.SetNewline(false);
    {
      auto sub_obj = writer.MakeObjectContext();
      writer.AddObjKey("k1");
      writer.Add(1);
      writer.AddObjKey("k2");
      writer.Add(2);
    }
    writer.SetNewline(true);
  }

  const std::string& json = writer.GetString();
  EXPECT_EQ(
      R"json({
  "bool_true": true,
  "bool_false": false,
  "null_val": null,
  "c_str": "hello",
  "std_str": "world",
  "sv_escaped": "quote\"bs\\b\bf\fn\nr\rt\tctrl\u0001",
  "raw_json": {"pre":1},
  "int32": -42,
  "uint32": 42,
  "int64": -9000000000,
  "uint64": 18000000000,
  "empty_obj": {},
  "empty_arr": [],
  "multiline_arr": [
    1,
    2
  ],
  "singleline_arr": [10, 20],
  "singleline_obj": {"k1": 1, "k2": 2}
})json",
      json);

  writer.clear();
  EXPECT_TRUE(writer.GetString().empty());
}

TEST(CplJsonStreamingWriterTest, FloatAndDoubleDefaultAndCustomPrecision) {
  constexpr float kFloat = 1.12345678901234567890f;
  constexpr double kDouble = 1.12345678901234567890;

  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(kFloat);
  EXPECT_EQ("1.12345684", writer.GetString());
  writer.clear();

  writer.Add(kFloat, 2);
  EXPECT_EQ("1.1", writer.GetString());
  writer.clear();

  writer.Add(kDouble);
  EXPECT_EQ("1.1234567890123457", writer.GetString());
  writer.clear();

  writer.Add(kDouble, 5);
  EXPECT_EQ("1.1235", writer.GetString());
}

TEST(CplJsonStreamingWriterTest, NonPrettyAndCallbackAndFloatingPoint) {
  std::string out;
  CPLJSonStreamingWriter writer(AppendToStringCallback, &out);
  writer.SetPrettyFormatting(false);

  {
    auto obj = writer.MakeObjectContext();
    writer.AddObjKey("floats");
    {
      auto arr = writer.MakeArrayContext();
      // GFloat16: finite, NaN, +Inf, -Inf
      writer.Add(GFloat16(1.5f), 4);
      writer.Add(GFloat16(std::numeric_limits<float>::quiet_NaN()));
      writer.Add(GFloat16(std::numeric_limits<float>::infinity()));
      writer.Add(GFloat16(-std::numeric_limits<float>::infinity()));

      // float: finite, NaN, +Inf, -Inf
      writer.Add(2.5f, 6);
      writer.Add(std::numeric_limits<float>::quiet_NaN());
      writer.Add(std::numeric_limits<float>::infinity());
      writer.Add(-std::numeric_limits<float>::infinity());

      // double: finite, NaN, +Inf, -Inf
      writer.Add(3.5, 10);
      writer.Add(std::numeric_limits<double>::quiet_NaN());
      writer.Add(std::numeric_limits<double>::infinity());
      writer.Add(-std::numeric_limits<double>::infinity());
    }
    writer.AddObjKey("second_key");
    writer.Add(1);
  }

  EXPECT_EQ(
      out,
      "{\"floats\":[1.5,\"NaN\",\"Infinity\",\"-Infinity\",2.5,\"NaN\","
      "\"Infinity\",\"-Infinity\",3.5,\"NaN\",\"Infinity\",\"-Infinity\"],"
      "\"second_key\":1}");
}

}  // namespace
}  // namespace autotest2
