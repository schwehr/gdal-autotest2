// Tests for GDAL's included json-c parsers called libjson internally to GDAL.
//
//   https://github.com/json-c/json-c/tree/master/tests
//
// Copyright 2015 Google Inc. All Rights Reserved.
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

#include "gunit.h"
#include "ogr/ogrsf_frmts/geojson/libjson/json.h"
#include "ogr/ogrsf_frmts/geojson/libjson/json_object.h"
#include "ogr/ogrsf_frmts/geojson/libjson/symbol_renames.h"

// #include <json.h>

namespace autotest2 {
namespace {

TEST(JsonCTest, BasicTypes) {
  // Strings
  json_object *obj = json_object_new_string("");
  EXPECT_STREQ("", json_object_get_string(obj));
  EXPECT_STREQ("\"\"", json_object_to_json_string(obj));
  json_object_put(obj);  // Decrement reference count on obj.

  obj = json_object_new_string("\t");
  EXPECT_STREQ("\t", json_object_get_string(obj));
  EXPECT_STREQ("\"\\t\"", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_string("\\");
  EXPECT_STREQ("\\", json_object_get_string(obj));
  EXPECT_STREQ("\"\\\\\"", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_string("foo");
  EXPECT_STREQ("foo", json_object_get_string(obj));
  EXPECT_STREQ("\"foo\"", json_object_to_json_string(obj));
  json_object_put(obj);

  // Integers.
  obj = json_object_new_int(0);
  EXPECT_STREQ("0", json_object_get_string(obj));
  EXPECT_STREQ("0", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_int(42);
  EXPECT_STREQ("42", json_object_get_string(obj));
  EXPECT_STREQ("42", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_int(-1);
  EXPECT_STREQ("-1", json_object_get_string(obj));
  EXPECT_STREQ("-1", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_int64(0);
  EXPECT_STREQ("0", json_object_get_string(obj));
  EXPECT_STREQ("0", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_int64(21987654321);
  EXPECT_STREQ("21987654321", json_object_get_string(obj));
  EXPECT_STREQ("21987654321", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_int64(-21987654321);
  EXPECT_STREQ("-21987654321", json_object_get_string(obj));
  EXPECT_STREQ("-21987654321", json_object_to_json_string(obj));
  json_object_put(obj);

  // Floating point.
  obj = json_object_new_int64(0.0);
  EXPECT_STREQ("0", json_object_get_string(obj));
  EXPECT_STREQ("0", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_double(0.1);
  EXPECT_STREQ("0.100000", json_object_get_string(obj));
  EXPECT_STREQ("0.100000", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_double(-0.2);
  EXPECT_STREQ("-0.200000", json_object_get_string(obj));
  EXPECT_STREQ("-0.200000", json_object_to_json_string(obj));
  json_object_put(obj);

  // Booleans
  obj = json_object_new_boolean(false);
  EXPECT_STREQ("false", json_object_get_string(obj));
  EXPECT_STREQ("false", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_boolean(true);
  EXPECT_STREQ("true", json_object_get_string(obj));
  EXPECT_STREQ("true", json_object_to_json_string(obj));
  json_object_put(obj);

  obj = json_object_new_array();
  json_object_array_add(obj, json_object_new_string("bar"));
  json_object_array_add(obj, json_object_new_boolean(true));
  json_object_array_add(obj, json_object_new_int(-9));
  json_object_array_add(obj, json_object_new_int64(-99));
  json_object_array_add(obj, json_object_new_double(-123.1));
  EXPECT_EQ(5, json_object_array_length(obj));
  EXPECT_STREQ(
      "[ \"bar\", true, -9, -99, -123.100000 ]",
      json_object_get_string(obj));
  EXPECT_STREQ(
      "[ \"bar\", true, -9, -99, -123.100000 ]",
      json_object_to_json_string(obj));
  json_object_put(obj);
}

} // namespace
} // namespace autotest2
