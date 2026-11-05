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

#include "third_party/gdal/ogr/ogrlibjsonutils.h"

#include <cstdint>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/json_c/json_object.h"
#include "third_party/json_c/json_types.h"

namespace autotest2 {
namespace {

TEST(OgrLibJsonUtilsTest, ParseJson) {
  json_object* obj = nullptr;

  // Null output pointer
  EXPECT_FALSE(OGRJSonParse("{}", nullptr));

  // Null input text
  EXPECT_FALSE(OGRJSonParse(nullptr, &obj, /*bVerboseError=*/false));
  EXPECT_EQ(nullptr, obj);

  // Invalid JSON with verbose error
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(OGRJSonParse("{ invalid", &obj, /*bVerboseError=*/true));
    EXPECT_EQ(nullptr, obj);
  }

  // Invalid JSON without verbose error
  EXPECT_FALSE(OGRJSonParse("{ invalid", &obj, /*bVerboseError=*/false));
  EXPECT_EQ(nullptr, obj);

  // Valid JSON
  EXPECT_TRUE(OGRJSonParse("{\"key\": \"value\", \"nested\": {\"x\": 10}}",
                           &obj, /*bVerboseError=*/true));
  ASSERT_NE(nullptr, obj);
  json_object_put(obj);
}

TEST(OgrLibJsonUtilsTest, ObjectByPathAndFindMember) {
  json_object* obj = nullptr;
  ASSERT_TRUE(OGRJSonParse(
      "{\"a\": {\"b\": 123, \"leaf\": \"txt\"}, \"arr\": [1, 2]}", &obj));
  ASSERT_NE(nullptr, obj);

  // CPL_json_object_object_get
  json_object* child = CPL_json_object_object_get(obj, "a");
  ASSERT_NE(nullptr, child);
  EXPECT_EQ(nullptr, CPL_json_object_object_get(obj, "nonexistent"));

  // json_ex_get_object_by_path null/empty inputs
  EXPECT_EQ(nullptr, json_ex_get_object_by_path(nullptr, "a.b"));
  EXPECT_EQ(nullptr, json_ex_get_object_by_path(obj, nullptr));
  EXPECT_EQ(nullptr, json_ex_get_object_by_path(obj, ""));

  // json_ex_get_object_by_path non-object root
  json_object* int_obj = json_object_new_int(42);
  EXPECT_EQ(nullptr, json_ex_get_object_by_path(int_obj, "a.b"));
  json_object_put(int_obj);

  // Valid nested path
  json_object* b = json_ex_get_object_by_path(obj, "a.b");
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(123, json_object_get_int(b));

  // Path reaching a non-existent key
  EXPECT_EQ(nullptr, json_ex_get_object_by_path(obj, "a.missing"));

  // Path with intermediate non-object
  EXPECT_EQ(nullptr, json_ex_get_object_by_path(obj, "arr.x"));

  // OGRGeoJSONFindMemberEntryByName and OGRGeoJSONFindMemberByName
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberEntryByName(nullptr, "a"));
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberEntryByName(obj, nullptr));
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberByName(nullptr, "a"));
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberByName(obj, nullptr));

  // Find member on non-object
  json_object* arr_obj = json_object_new_array();
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberEntryByName(arr_obj, "a"));
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberByName(arr_obj, "a"));
  json_object_put(arr_obj);

  // Existing and non-existing members
  EXPECT_NE(nullptr, OGRGeoJSONFindMemberEntryByName(obj, "a"));
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberEntryByName(obj, "missing"));

  json_object* found = OGRGeoJSONFindMemberByName(obj, "a");
  ASSERT_NE(nullptr, found);
  EXPECT_EQ(child, found);
  EXPECT_EQ(nullptr, OGRGeoJSONFindMemberByName(obj, "missing"));

  json_object_put(obj);
}

TEST(OgrLibJsonUtilsTest, DoubleWithPrecision) {
  // Normal precision
  json_object* jso = json_object_new_double_with_precision(12.34567, 2);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("12.35", json_object_to_json_string(jso));
  json_object_put(jso);

  // Negative precision (defaults to 15)
  jso = json_object_new_double_with_precision(1.25, -1);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("1.25", json_object_to_json_string(jso));
  json_object_put(jso);

  // Huge double > 1e50
  jso = json_object_new_double_with_precision(1e55, 3);
  ASSERT_NE(nullptr, jso);
  std::string s = json_object_to_json_string(jso);
  EXPECT_NE(std::string::npos, s.find("e+55"));
  json_object_put(jso);
}

TEST(OgrLibJsonUtilsTest, DoubleWithSignificantFigures) {
  // NaN
  json_object* jso = json_object_new_double_with_significant_figures(
      std::numeric_limits<double>::quiet_NaN(), 6);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("NaN", json_object_to_json_string(jso));
  json_object_put(jso);

  // +Infinity
  jso = json_object_new_double_with_significant_figures(
      std::numeric_limits<double>::infinity(), 6);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("Infinity", json_object_to_json_string(jso));
  json_object_put(jso);

  // -Infinity
  jso = json_object_new_double_with_significant_figures(
      -std::numeric_limits<double>::infinity(), 6);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("-Infinity", json_object_to_json_string(jso));
  json_object_put(jso);

  // Integer-valued double (appends .0)
  jso = json_object_new_double_with_significant_figures(42.0, 6);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("42.0", json_object_to_json_string(jso));
  json_object_put(jso);

  // Negative significant figures (defaults to 17)
  jso = json_object_new_double_with_significant_figures(123.456, -1);
  ASSERT_NE(nullptr, jso);
  EXPECT_STREQ("123.456", json_object_to_json_string(jso));
  json_object_put(jso);

  // Value containing 000000 or 999999 to trigger precision reduction loop
  jso = json_object_new_double_with_significant_figures(1.000000123456789, 15);
  ASSERT_NE(nullptr, jso);
  EXPECT_NE(nullptr, json_object_to_json_string(jso));
  json_object_put(jso);

  // Value where all 3 retries still have 000000 or 999999
  jso = json_object_new_double_with_significant_figures(1.0000000000001, 15);
  ASSERT_NE(nullptr, jso);
  EXPECT_NE(nullptr, json_object_to_json_string(jso));
  json_object_put(jso);
}

TEST(OgrLibJsonUtilsTest, PropertyToFieldTypeScalars) {
  OGRFieldSubType sub_type = OFSTNone;

  // Null object
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(nullptr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);

  // Boolean
  json_object* b = json_object_new_boolean(true);
  EXPECT_EQ(OFTInteger, GeoJSONPropertyToFieldType(b, sub_type));
  EXPECT_EQ(OFSTBoolean, sub_type);
  json_object_put(b);

  // Double
  json_object* d = json_object_new_double(3.14);
  EXPECT_EQ(OFTReal, GeoJSONPropertyToFieldType(d, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(d);

  // Int32
  json_object* i32 = json_object_new_int(12345);
  EXPECT_EQ(OFTInteger, GeoJSONPropertyToFieldType(i32, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(i32);

  // Int64
  int64_t large_val = (1LL << 35);
  json_object* i64 = json_object_new_int64(large_val);
  EXPECT_EQ(OFTInteger64, GeoJSONPropertyToFieldType(i64, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(i64);

  // Extreme int64 min / max
  constexpr GIntBig kMyInt64Max =
      (static_cast<GIntBig>(0x7FFFFFFF) << 32) | 0xFFFFFFFF;
  json_object* i64_max = json_object_new_int64(kMyInt64Max);
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OFTInteger64, GeoJSONPropertyToFieldType(i64_max, sub_type));
  }
  json_object_put(i64_max);

  // String
  json_object* s = json_object_new_string("hello");
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(s, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(s);

  // Object
  json_object* o = json_object_new_object();
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(o, sub_type));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(o);
}

TEST(OgrLibJsonUtilsTest, PropertyToFieldTypeArrays) {
  OGRFieldSubType sub_type = OFSTNone;

  // bArrayAsString = true
  json_object* arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_int(1));
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(arr, sub_type,
                                                  /*bArrayAsString=*/true));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(arr);

  // Empty array
  arr = json_object_new_array();
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(arr);

  // Array of strings
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_string("a"));
  json_object_array_add(arr, json_object_new_string("b"));
  EXPECT_EQ(OFTStringList, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(arr);

  // Array of strings followed by non-string (mixed -> JSON string)
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_int(1));
  json_object_array_add(arr, json_object_new_string("b"));
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(arr);

  // Array of doubles
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_double(1.5));
  json_object_array_add(arr, json_object_new_double(2.5));
  EXPECT_EQ(OFTRealList, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(arr);

  // Array of int32 then double -> OFTRealList
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_int(1));
  json_object_array_add(arr, json_object_new_double(2.5));
  EXPECT_EQ(OFTRealList, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(arr);

  // Array of int32
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_int(1));
  json_object_array_add(arr, json_object_new_int(2));
  EXPECT_EQ(OFTIntegerList, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(arr);

  // Array of int32 then int64
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_int(1));
  json_object_array_add(arr, json_object_new_int64(1LL << 35));
  EXPECT_EQ(OFTInteger64List, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(arr);

  // Array of int64 then int32
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_int64(1LL << 35));
  json_object_array_add(arr, json_object_new_int(2));
  EXPECT_EQ(OFTInteger64List, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTNone, sub_type);
  json_object_put(arr);

  // Array of booleans
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_boolean(true));
  json_object_array_add(arr, json_object_new_boolean(false));
  EXPECT_EQ(OFTIntegerList, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTBoolean, sub_type);
  json_object_put(arr);

  // Array of booleans mixed with double
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_boolean(true));
  json_object_array_add(arr, json_object_new_double(1.5));
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(arr);

  // Array of booleans mixed with int
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_boolean(true));
  json_object_array_add(arr, json_object_new_int(1));
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(arr);

  // Array with nested object
  arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_object());
  EXPECT_EQ(OFTString, GeoJSONPropertyToFieldType(arr, sub_type));
  EXPECT_EQ(OFSTJSON, sub_type);
  json_object_put(arr);
}

}  // namespace
}  // namespace autotest2
