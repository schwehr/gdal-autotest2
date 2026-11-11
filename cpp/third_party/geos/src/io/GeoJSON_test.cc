// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/io/GeoJSON.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"

namespace geos_google {
namespace {

enum class ExpectedKind {
  kNumber,
  kString,
  kNull,
  kBoolean,
  kObject,
  kArray,
};

void ExpectKindAndThrowOnOthers(const geos::io::GeoJSONValue& v,
                                ExpectedKind expected) {
  EXPECT_EQ(v.isNumber(), expected == ExpectedKind::kNumber);
  EXPECT_EQ(v.isString(), expected == ExpectedKind::kString);
  EXPECT_EQ(v.isNull(), expected == ExpectedKind::kNull);
  EXPECT_EQ(v.isBoolean(), expected == ExpectedKind::kBoolean);
  EXPECT_EQ(v.isObject(), expected == ExpectedKind::kObject);
  EXPECT_EQ(v.isArray(), expected == ExpectedKind::kArray);

  if (expected != ExpectedKind::kNumber) {
    EXPECT_THROW((void)v.getNumber(), geos::io::GeoJSONValue::GeoJSONTypeError);
  }
  if (expected != ExpectedKind::kString) {
    EXPECT_THROW((void)v.getString(), geos::io::GeoJSONValue::GeoJSONTypeError);
  }
  if (expected != ExpectedKind::kNull) {
    EXPECT_THROW((void)v.getNull(), geos::io::GeoJSONValue::GeoJSONTypeError);
  }
  if (expected != ExpectedKind::kBoolean) {
    EXPECT_THROW((void)v.getBoolean(),
                 geos::io::GeoJSONValue::GeoJSONTypeError);
  }
  if (expected != ExpectedKind::kObject) {
    EXPECT_THROW((void)v.getObject(), geos::io::GeoJSONValue::GeoJSONTypeError);
  }
  if (expected != ExpectedKind::kArray) {
    EXPECT_THROW((void)v.getArray(), geos::io::GeoJSONValue::GeoJSONTypeError);
  }
}

TEST(GeoJSONValueTest, NumberTypeAndErrors) {
  const geos::io::GeoJSONValue zero(0.0);
  ExpectKindAndThrowOnOthers(zero, ExpectedKind::kNumber);
  EXPECT_DOUBLE_EQ(zero.getNumber(), 0.0);

  const geos::io::GeoJSONValue negative(-42.5);
  ExpectKindAndThrowOnOthers(negative, ExpectedKind::kNumber);
  EXPECT_DOUBLE_EQ(negative.getNumber(), -42.5);

  const geos::io::GeoJSONValue pi(3.141592653589793);
  ExpectKindAndThrowOnOthers(pi, ExpectedKind::kNumber);
  EXPECT_DOUBLE_EQ(pi.getNumber(), 3.141592653589793);

  const geos::io::GeoJSONValue nan_val(
      std::numeric_limits<double>::quiet_NaN());
  ExpectKindAndThrowOnOthers(nan_val, ExpectedKind::kNumber);
  EXPECT_TRUE(std::isnan(nan_val.getNumber()));

  const geos::io::GeoJSONValue inf_val(std::numeric_limits<double>::infinity());
  ExpectKindAndThrowOnOthers(inf_val, ExpectedKind::kNumber);
  EXPECT_TRUE(std::isinf(inf_val.getNumber()));
}

TEST(GeoJSONValueTest, StringTypeAndErrors) {
  const geos::io::GeoJSONValue empty(std::string(""));
  ExpectKindAndThrowOnOthers(empty, ExpectedKind::kString);
  EXPECT_EQ(empty.getString(), "");

  const geos::io::GeoJSONValue text(std::string("geojson-value"));
  ExpectKindAndThrowOnOthers(text, ExpectedKind::kString);
  EXPECT_EQ(text.getString(), "geojson-value");
}

TEST(GeoJSONValueTest, NullTypeAndErrors) {
  const geos::io::GeoJSONValue null_val;
  ExpectKindAndThrowOnOthers(null_val, ExpectedKind::kNull);
  const std::nullptr_t n = null_val.getNull();
  EXPECT_TRUE(n == nullptr);
}

TEST(GeoJSONValueTest, BooleanTypeAndErrors) {
  const geos::io::GeoJSONValue t(true);
  ExpectKindAndThrowOnOthers(t, ExpectedKind::kBoolean);
  EXPECT_TRUE(t.getBoolean());

  const geos::io::GeoJSONValue f(false);
  ExpectKindAndThrowOnOthers(f, ExpectedKind::kBoolean);
  EXPECT_FALSE(f.getBoolean());
}

TEST(GeoJSONValueTest, ObjectTypeAndErrors) {
  const geos::io::GeoJSONValue empty_obj(
      std::map<std::string, geos::io::GeoJSONValue>{});
  ExpectKindAndThrowOnOthers(empty_obj, ExpectedKind::kObject);
  EXPECT_TRUE(empty_obj.getObject().empty());

  const std::map<std::string, geos::io::GeoJSONValue> nested_map = {
      {"inner", geos::io::GeoJSONValue(std::string("value"))},
  };
  const std::vector<geos::io::GeoJSONValue> nested_vec = {
      geos::io::GeoJSONValue(1.0),
      geos::io::GeoJSONValue(true),
  };
  const std::map<std::string, geos::io::GeoJSONValue> obj_map = {
      {"num", geos::io::GeoJSONValue(7.5)},
      {"str", geos::io::GeoJSONValue(std::string("abc"))},
      {"nil", geos::io::GeoJSONValue()},
      {"flag", geos::io::GeoJSONValue(false)},
      {"obj", geos::io::GeoJSONValue(nested_map)},
      {"arr", geos::io::GeoJSONValue(nested_vec)},
  };

  const geos::io::GeoJSONValue obj(obj_map);
  ExpectKindAndThrowOnOthers(obj, ExpectedKind::kObject);
  const auto& result = obj.getObject();
  ASSERT_EQ(result.size(), 6u);
  EXPECT_DOUBLE_EQ(result.at("num").getNumber(), 7.5);
  EXPECT_EQ(result.at("str").getString(), "abc");
  EXPECT_TRUE(result.at("nil").isNull());
  EXPECT_FALSE(result.at("flag").getBoolean());
  EXPECT_EQ(result.at("obj").getObject().at("inner").getString(), "value");
  ASSERT_EQ(result.at("arr").getArray().size(), 2u);
  EXPECT_DOUBLE_EQ(result.at("arr").getArray()[0].getNumber(), 1.0);
  EXPECT_TRUE(result.at("arr").getArray()[1].getBoolean());
}

TEST(GeoJSONValueTest, ArrayTypeAndErrors) {
  const geos::io::GeoJSONValue empty_arr(std::vector<geos::io::GeoJSONValue>{});
  ExpectKindAndThrowOnOthers(empty_arr, ExpectedKind::kArray);
  EXPECT_TRUE(empty_arr.getArray().empty());

  const std::map<std::string, geos::io::GeoJSONValue> sub_obj = {
      {"k", geos::io::GeoJSONValue(99.0)},
  };
  const std::vector<geos::io::GeoJSONValue> elements = {
      geos::io::GeoJSONValue(2.5),
      geos::io::GeoJSONValue(std::string("item")),
      geos::io::GeoJSONValue(),
      geos::io::GeoJSONValue(true),
      geos::io::GeoJSONValue(sub_obj),
      geos::io::GeoJSONValue(std::vector<geos::io::GeoJSONValue>{
          geos::io::GeoJSONValue(std::string("nested")),
      }),
  };

  const geos::io::GeoJSONValue arr(elements);
  ExpectKindAndThrowOnOthers(arr, ExpectedKind::kArray);
  const auto& result = arr.getArray();
  ASSERT_EQ(result.size(), 6u);
  EXPECT_DOUBLE_EQ(result[0].getNumber(), 2.5);
  EXPECT_EQ(result[1].getString(), "item");
  EXPECT_TRUE(result[2].isNull());
  EXPECT_TRUE(result[3].getBoolean());
  EXPECT_DOUBLE_EQ(result[4].getObject().at("k").getNumber(), 99.0);
  ASSERT_EQ(result[5].getArray().size(), 1u);
  EXPECT_EQ(result[5].getArray()[0].getString(), "nested");
}

TEST(GeoJSONValueTest, CopyConstructorAllTypes) {
  const geos::io::GeoJSONValue num_src(12.25);
  const geos::io::GeoJSONValue num_copy(num_src);
  EXPECT_TRUE(num_copy.isNumber());
  EXPECT_DOUBLE_EQ(num_copy.getNumber(), 12.25);

  const geos::io::GeoJSONValue bool_src(true);
  const geos::io::GeoJSONValue bool_copy(bool_src);
  EXPECT_TRUE(bool_copy.isBoolean());
  EXPECT_TRUE(bool_copy.getBoolean());

  const geos::io::GeoJSONValue str_src(std::string("copied-string"));
  const geos::io::GeoJSONValue str_copy(str_src);
  EXPECT_TRUE(str_copy.isString());
  EXPECT_EQ(str_copy.getString(), "copied-string");
  EXPECT_NE(&str_copy.getString(), &str_src.getString());

  const geos::io::GeoJSONValue null_src;
  const geos::io::GeoJSONValue null_copy(null_src);
  EXPECT_TRUE(null_copy.isNull());
  EXPECT_TRUE(null_copy.getNull() == nullptr);

  const geos::io::GeoJSONValue obj_src(
      std::map<std::string, geos::io::GeoJSONValue>{
          {"a", geos::io::GeoJSONValue(1.0)},
          {"b", geos::io::GeoJSONValue(std::string("two"))},
      });
  const geos::io::GeoJSONValue obj_copy(obj_src);
  EXPECT_TRUE(obj_copy.isObject());
  EXPECT_NE(&obj_copy.getObject(), &obj_src.getObject());
  ASSERT_EQ(obj_copy.getObject().size(), 2u);
  EXPECT_DOUBLE_EQ(obj_copy.getObject().at("a").getNumber(), 1.0);
  EXPECT_EQ(obj_copy.getObject().at("b").getString(), "two");

  const geos::io::GeoJSONValue empty_arr_src(
      std::vector<geos::io::GeoJSONValue>{});
  const geos::io::GeoJSONValue empty_arr_copy(empty_arr_src);
  EXPECT_TRUE(empty_arr_copy.isArray());
  EXPECT_TRUE(empty_arr_copy.getArray().empty());

  const geos::io::GeoJSONValue arr_src(std::vector<geos::io::GeoJSONValue>{
      geos::io::GeoJSONValue(3.0),
      geos::io::GeoJSONValue(false),
      geos::io::GeoJSONValue(std::string("el")),
  });
  const geos::io::GeoJSONValue arr_copy(arr_src);
  EXPECT_TRUE(arr_copy.isArray());
  EXPECT_NE(&arr_copy.getArray(), &arr_src.getArray());
  ASSERT_EQ(arr_copy.getArray().size(), 3u);
  EXPECT_DOUBLE_EQ(arr_copy.getArray()[0].getNumber(), 3.0);
  EXPECT_FALSE(arr_copy.getArray()[1].getBoolean());
  EXPECT_EQ(arr_copy.getArray()[2].getString(), "el");
}

TEST(GeoJSONValueTest, CopyAssignmentSameTypeAndSelf) {
  // STRING = STRING
  geos::io::GeoJSONValue s1(std::string("first"));
  const geos::io::GeoJSONValue s2(std::string("second"));
  s1 = s2;
  EXPECT_TRUE(s1.isString());
  EXPECT_EQ(s1.getString(), "second");
  geos::io::GeoJSONValue* s1_ptr = &s1;
  s1 = *s1_ptr;
  EXPECT_EQ(s1.getString(), "second");

  // OBJECT = OBJECT
  geos::io::GeoJSONValue o1(std::map<std::string, geos::io::GeoJSONValue>{
      {"old", geos::io::GeoJSONValue(1.0)},
  });
  const geos::io::GeoJSONValue o2(std::map<std::string, geos::io::GeoJSONValue>{
      {"new", geos::io::GeoJSONValue(2.0)},
  });
  o1 = o2;
  EXPECT_TRUE(o1.isObject());
  EXPECT_EQ(o1.getObject().count("old"), 0u);
  EXPECT_DOUBLE_EQ(o1.getObject().at("new").getNumber(), 2.0);
  geos::io::GeoJSONValue* o1_ptr = &o1;
  o1 = *o1_ptr;
  EXPECT_DOUBLE_EQ(o1.getObject().at("new").getNumber(), 2.0);

  // ARRAY = ARRAY
  geos::io::GeoJSONValue a1(std::vector<geos::io::GeoJSONValue>{
      geos::io::GeoJSONValue(1.0),
  });
  const geos::io::GeoJSONValue a2(std::vector<geos::io::GeoJSONValue>{
      geos::io::GeoJSONValue(10.0),
      geos::io::GeoJSONValue(20.0),
  });
  a1 = a2;
  EXPECT_TRUE(a1.isArray());
  ASSERT_EQ(a1.getArray().size(), 2u);
  EXPECT_DOUBLE_EQ(a1.getArray()[0].getNumber(), 10.0);
  EXPECT_DOUBLE_EQ(a1.getArray()[1].getNumber(), 20.0);
  geos::io::GeoJSONValue* a1_ptr = &a1;
  a1 = *a1_ptr;
  ASSERT_EQ(a1.getArray().size(), 2u);

  // Primitive / null self-assignment and chained assignment
  geos::io::GeoJSONValue n1(5.0);
  geos::io::GeoJSONValue n2(8.0);
  geos::io::GeoJSONValue n3(13.0);
  n1 = n2 = n3;
  EXPECT_DOUBLE_EQ(n1.getNumber(), 13.0);
  EXPECT_DOUBLE_EQ(n2.getNumber(), 13.0);
  geos::io::GeoJSONValue* n1_ptr = &n1;
  n1 = *n1_ptr;
  EXPECT_DOUBLE_EQ(n1.getNumber(), 13.0);

  geos::io::GeoJSONValue b1(false);
  b1 = geos::io::GeoJSONValue(true);
  EXPECT_TRUE(b1.getBoolean());

  geos::io::GeoJSONValue nil1;
  const geos::io::GeoJSONValue nil2;
  nil1 = nil2;
  EXPECT_TRUE(nil1.isNull());
}

TEST(GeoJSONValueTest, CopyAssignmentCrossTypeTransitions) {
  geos::io::GeoJSONValue val;
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kNull);

  // NULLTYPE -> ARRAY (non-empty)
  val = geos::io::GeoJSONValue(std::vector<geos::io::GeoJSONValue>{
      geos::io::GeoJSONValue(1.0),
      geos::io::GeoJSONValue(std::string("two")),
  });
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kArray);
  ASSERT_EQ(val.getArray().size(), 2u);
  EXPECT_DOUBLE_EQ(val.getArray()[0].getNumber(), 1.0);
  EXPECT_EQ(val.getArray()[1].getString(), "two");

  // ARRAY -> OBJECT
  val = geos::io::GeoJSONValue(std::map<std::string, geos::io::GeoJSONValue>{
      {"key", geos::io::GeoJSONValue(true)},
  });
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kObject);
  EXPECT_TRUE(val.getObject().at("key").getBoolean());

  // OBJECT -> BOOLEAN
  val = geos::io::GeoJSONValue(false);
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kBoolean);
  EXPECT_FALSE(val.getBoolean());

  // BOOLEAN -> STRING
  val = geos::io::GeoJSONValue(std::string("switched"));
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kString);
  EXPECT_EQ(val.getString(), "switched");

  // STRING -> NUMBER
  val = geos::io::GeoJSONValue(-9.25);
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kNumber);
  EXPECT_DOUBLE_EQ(val.getNumber(), -9.25);

  // NUMBER -> NULLTYPE
  val = geos::io::GeoJSONValue();
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kNull);
  EXPECT_TRUE(val.getNull() == nullptr);

  // NULLTYPE -> OBJECT -> STRING -> ARRAY -> NULLTYPE
  val = geos::io::GeoJSONValue(std::map<std::string, geos::io::GeoJSONValue>{
      {"x", geos::io::GeoJSONValue(4.0)},
  });
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kObject);

  val = geos::io::GeoJSONValue(std::string("after-object"));
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kString);
  EXPECT_EQ(val.getString(), "after-object");

  val = geos::io::GeoJSONValue(std::vector<geos::io::GeoJSONValue>{});
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kArray);
  EXPECT_TRUE(val.getArray().empty());

  val = geos::io::GeoJSONValue();
  ExpectKindAndThrowOnOthers(val, ExpectedKind::kNull);
}

TEST(GeoJSONFeatureTest, ConstructorsAndGetters) {
  auto factory = geos::geom::GeometryFactory::create();

  // 1) (unique_ptr<Geometry>, const map&)
  const std::map<std::string, geos::io::GeoJSONValue> props_lvalue = {
      {"name", geos::io::GeoJSONValue(std::string("point-a"))},
      {"count", geos::io::GeoJSONValue(3.0)},
  };
  auto pt1 = factory->createPoint(geos::geom::CoordinateXY(1.0, 2.0));
  const geos::io::GeoJSONFeature f1(std::move(pt1), props_lvalue);
  ASSERT_NE(f1.getGeometry(), nullptr);
  EXPECT_TRUE(f1.getGeometry()->equalsExact(
      factory->createPoint(geos::geom::CoordinateXY(1.0, 2.0)).get()));
  EXPECT_EQ(f1.getProperties().at("name").getString(), "point-a");
  EXPECT_DOUBLE_EQ(f1.getProperties().at("count").getNumber(), 3.0);
  EXPECT_EQ(f1.getId(), "");
  EXPECT_EQ(props_lvalue.size(), 2u);

  // 2) (unique_ptr<Geometry>, map&&)
  std::map<std::string, geos::io::GeoJSONValue> props_rvalue2 = {
      {"active", geos::io::GeoJSONValue(true)},
  };
  auto seq = std::make_unique<geos::geom::CoordinateSequence>(
      std::initializer_list<geos::geom::CoordinateXY>{
          geos::geom::CoordinateXY(0.0, 0.0),
          geos::geom::CoordinateXY(5.0, 5.0),
      });
  auto line = factory->createLineString(std::move(seq));
  const geos::io::GeoJSONFeature f2(std::move(line), std::move(props_rvalue2));
  ASSERT_NE(f2.getGeometry(), nullptr);
  EXPECT_EQ(f2.getGeometry()->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(f2.getGeometry()->getNumPoints(), 2u);
  EXPECT_TRUE(f2.getProperties().at("active").getBoolean());
  EXPECT_EQ(f2.getId(), "");

  // 3) (unique_ptr<Geometry>, const map&, const string&)
  const std::string id3 = "feature-3";
  auto pt3 = factory->createPoint(geos::geom::CoordinateXY(7.0, 8.0));
  const geos::io::GeoJSONFeature f3(std::move(pt3), props_lvalue, id3);
  ASSERT_NE(f3.getGeometry(), nullptr);
  EXPECT_TRUE(f3.getGeometry()->equalsExact(
      factory->createPoint(geos::geom::CoordinateXY(7.0, 8.0)).get()));
  EXPECT_EQ(f3.getProperties().at("name").getString(), "point-a");
  EXPECT_EQ(f3.getId(), "feature-3");

  // 4) (unique_ptr<Geometry>, map&&, string)
  std::map<std::string, geos::io::GeoJSONValue> props_rvalue4 = {
      {"tag", geos::io::GeoJSONValue(std::string("four"))},
  };
  std::string id4 = "feature-4";
  auto pt4 = factory->createPoint(geos::geom::CoordinateXY(9.0, 10.0));
  const geos::io::GeoJSONFeature f4(std::move(pt4), std::move(props_rvalue4),
                                    id4);
  ASSERT_NE(f4.getGeometry(), nullptr);
  EXPECT_TRUE(f4.getGeometry()->equalsExact(
      factory->createPoint(geos::geom::CoordinateXY(9.0, 10.0)).get()));
  EXPECT_EQ(f4.getProperties().at("tag").getString(), "four");
  EXPECT_EQ(f4.getId(), "feature-4");

  // Null geometry is permitted by constructors and returned as nullptr.
  const geos::io::GeoJSONFeature null_geom_feature(
      nullptr, std::map<std::string, geos::io::GeoJSONValue>{},
      std::string("null-geom"));
  EXPECT_EQ(null_geom_feature.getGeometry(), nullptr);
  EXPECT_TRUE(null_geom_feature.getProperties().empty());
  EXPECT_EQ(null_geom_feature.getId(), "null-geom");
}

TEST(GeoJSONFeatureTest, CopyAndMoveSemantics) {
  auto factory = geos::geom::GeometryFactory::create();
  auto expected_orig =
      factory->createPoint(geos::geom::CoordinateXY(11.0, 22.0));
  auto expected_other =
      factory->createPoint(geos::geom::CoordinateXY(33.0, 44.0));

  const std::map<std::string, geos::io::GeoJSONValue> orig_props = {
      {"k", geos::io::GeoJSONValue(std::string("v"))},
  };
  geos::io::GeoJSONFeature orig(
      factory->createPoint(geos::geom::CoordinateXY(11.0, 22.0)), orig_props,
      std::string("orig-id"));

  // Copy constructor deep-clones geometry and copies properties and id.
  geos::io::GeoJSONFeature copied(orig);
  ASSERT_NE(copied.getGeometry(), nullptr);
  EXPECT_NE(copied.getGeometry(), orig.getGeometry());
  EXPECT_TRUE(copied.getGeometry()->equalsExact(expected_orig.get()));
  EXPECT_EQ(copied.getProperties().at("k").getString(), "v");
  EXPECT_EQ(copied.getId(), "orig-id");

  // Self copy-assignment returns *this cleanly without cloning over itself.
  const geos::geom::Geometry* copied_ptr = copied.getGeometry();
  geos::io::GeoJSONFeature* copied_alias = &copied;
  copied = *copied_alias;
  EXPECT_EQ(copied.getGeometry(), copied_ptr);
  EXPECT_EQ(copied.getProperties().at("k").getString(), "v");
  EXPECT_EQ(copied.getId(), "orig-id");

  // Copy assignment from another feature clones geometry and copies properties.
  geos::io::GeoJSONFeature target(
      factory->createPoint(geos::geom::CoordinateXY(33.0, 44.0)),
      std::map<std::string, geos::io::GeoJSONValue>{
          {"old", geos::io::GeoJSONValue(1.0)},
      },
      std::string("target-id"));
  target = copied;
  ASSERT_NE(target.getGeometry(), nullptr);
  EXPECT_NE(target.getGeometry(), copied.getGeometry());
  EXPECT_TRUE(target.getGeometry()->equalsExact(expected_orig.get()));
  EXPECT_EQ(target.getProperties().count("old"), 0u);
  EXPECT_EQ(target.getProperties().at("k").getString(), "v");

  // Move constructor transfers geometry, properties, and id.
  const geos::geom::Geometry* orig_raw = orig.getGeometry();
  geos::io::GeoJSONFeature moved(std::move(orig));
  EXPECT_EQ(moved.getGeometry(), orig_raw);
  EXPECT_EQ(orig.getGeometry(), nullptr);
  EXPECT_EQ(moved.getProperties().at("k").getString(), "v");
  EXPECT_EQ(moved.getId(), "orig-id");

  // Move assignment transfers geometry and properties.
  geos::io::GeoJSONFeature source(
      factory->createPoint(geos::geom::CoordinateXY(33.0, 44.0)),
      std::map<std::string, geos::io::GeoJSONValue>{
          {"moved_prop", geos::io::GeoJSONValue(99.0)},
      },
      std::string("source-id"));
  const geos::geom::Geometry* source_raw = source.getGeometry();
  target = std::move(source);
  EXPECT_EQ(target.getGeometry(), source_raw);
  EXPECT_EQ(source.getGeometry(), nullptr);
  EXPECT_TRUE(target.getGeometry()->equalsExact(expected_other.get()));
  EXPECT_DOUBLE_EQ(target.getProperties().at("moved_prop").getNumber(), 99.0);
}

TEST(GeoJSONFeatureCollectionTest, CopyAndMoveConstructors) {
  auto factory = geos::geom::GeometryFactory::create();

  const std::vector<geos::io::GeoJSONFeature> empty_vec;
  const geos::io::GeoJSONFeatureCollection empty_copy(empty_vec);
  EXPECT_TRUE(empty_copy.getFeatures().empty());

  std::vector<geos::io::GeoJSONFeature> empty_move_vec;
  const geos::io::GeoJSONFeatureCollection empty_moved(
      std::move(empty_move_vec));
  EXPECT_TRUE(empty_moved.getFeatures().empty());

  std::vector<geos::io::GeoJSONFeature> features;
  features.reserve(2);
  features.emplace_back(
      factory->createPoint(geos::geom::CoordinateXY(1.0, 2.0)),
      std::map<std::string, geos::io::GeoJSONValue>{
          {"index", geos::io::GeoJSONValue(0.0)},
      },
      std::string("f0"));
  features.emplace_back(
      factory->createEmptyGeometry(geos::geom::GEOS_POLYGON),
      std::map<std::string, geos::io::GeoJSONValue>{
          {"label", geos::io::GeoJSONValue(std::string("empty-poly"))},
      },
      std::string("f1"));

  const geos::geom::Geometry* first_raw = features[0].getGeometry();
  const geos::geom::Geometry* second_raw = features[1].getGeometry();

  // Copy constructor: clones geometries and preserves properties and ids.
  const geos::io::GeoJSONFeatureCollection copied_fc(features);
  ASSERT_EQ(copied_fc.getFeatures().size(), 2u);
  EXPECT_NE(copied_fc.getFeatures()[0].getGeometry(), first_raw);
  EXPECT_TRUE(copied_fc.getFeatures()[0].getGeometry()->equalsExact(first_raw));
  EXPECT_DOUBLE_EQ(
      copied_fc.getFeatures()[0].getProperties().at("index").getNumber(), 0.0);
  EXPECT_EQ(copied_fc.getFeatures()[0].getId(), "f0");

  EXPECT_NE(copied_fc.getFeatures()[1].getGeometry(), second_raw);
  EXPECT_TRUE(copied_fc.getFeatures()[1].getGeometry()->isEmpty());
  EXPECT_EQ(copied_fc.getFeatures()[1].getGeometry()->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_EQ(copied_fc.getFeatures()[1].getProperties().at("label").getString(),
            "empty-poly");
  EXPECT_EQ(copied_fc.getFeatures()[1].getId(), "f1");

  // Move constructor: moves vector ownership without cloning geometries.
  const geos::io::GeoJSONFeatureCollection moved_fc(std::move(features));
  ASSERT_EQ(moved_fc.getFeatures().size(), 2u);
  EXPECT_EQ(moved_fc.getFeatures()[0].getGeometry(), first_raw);
  EXPECT_EQ(moved_fc.getFeatures()[0].getId(), "f0");
  EXPECT_EQ(moved_fc.getFeatures()[1].getGeometry(), second_raw);
  EXPECT_EQ(moved_fc.getFeatures()[1].getId(), "f1");
}

}  // namespace
}  // namespace geos_google
