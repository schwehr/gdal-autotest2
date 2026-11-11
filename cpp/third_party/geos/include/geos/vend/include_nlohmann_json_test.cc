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

// Unit tests for include_nlohmann_json.hpp wrapper macro isolation and the
// vendored geos_nlohmann JSON library exposed through it.
// See also:
// https://github.com/libgeos/geos/blob/main/include/geos/vend/include_nlohmann_json.hpp

#include "third_party/geos/include/geos/vend/include_nlohmann_json.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <forward_list>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <valarray>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using ::geos_nlohmann::json;
using ::geos_nlohmann::ordered_json;
using ::testing::ElementsAre;

// Capture preprocessor state immediately after including
// include_nlohmann_json.hpp to verify guard definition and macro cleanup.
#ifdef GEOS_VEND_JSON
constexpr bool kHasGeosVendJsonGuard = true;
#else
constexpr bool kHasGeosVendJsonGuard = false;
#endif

#ifdef nlohmann
constexpr bool kNlohmannMacroLeaked = true;
#else
constexpr bool kNlohmannMacroLeaked = false;
#endif

#ifdef JSON_THROW
constexpr bool kJsonThrowLeaked = true;
#else
constexpr bool kJsonThrowLeaked = false;
#endif

#ifdef JSON_ASSERT
constexpr bool kJsonAssertLeaked = true;
#else
constexpr bool kJsonAssertLeaked = false;
#endif

#ifdef JSON_HEDLEY_VERSION
constexpr bool kJsonHedleyLeaked = true;
#else
constexpr bool kJsonHedleyLeaked = false;
#endif

#ifdef JSON_HAS_CPP_20
constexpr bool kJsonHasCpp20Leaked = true;
#else
constexpr bool kJsonHasCpp20Leaked = false;
#endif

#ifdef JSON_USE_GLOBAL_UDLS
constexpr bool kJsonUseGlobalUdlsLeaked = true;
#else
constexpr bool kJsonUseGlobalUdlsLeaked = false;
#endif

// Unrelated namespace named `nlohmann` to verify that include_nlohmann_json.hpp
// undefines `nlohmann` cleanly after vendoring into `geos_nlohmann`.
namespace nlohmann {
struct UnrelatedMarker {
  int code = 42;
};
}  // namespace nlohmann

struct Point2D {
  double x = 0.0;
  double y = 0.0;

  bool operator==(const Point2D& other) const = default;
};

void to_json(json& j, const Point2D& p) {
  j = json{{"x", p.x}, {"y", p.y}};
}

void from_json(const json& j, Point2D& p) {
  j.at("x").get_to(p.x);
  j.at("y").get_to(p.y);
}

enum class GeometryKind {
  kUnknown,
  kPoint,
  kLineString,
  kPolygon,
};

NLOHMANN_JSON_SERIALIZE_ENUM(GeometryKind,
                             {
                                 {GeometryKind::kUnknown, "unknown"},
                                 {GeometryKind::kPoint, "Point"},
                                 {GeometryKind::kLineString, "LineString"},
                                 {GeometryKind::kPolygon, "Polygon"},
                             })

// Macros such as NLOHMANN_DEFINE_TYPE_* expand `nlohmann::json` at call sites
// after include_nlohmann_json.hpp has `#undef nlohmann`ed it, so callers using
// those macros can introduce a local namespace alias.
namespace macro_compat {
namespace nlohmann = ::geos_nlohmann;

struct FeatureRecord {
  int id = 0;
  std::string label = "default";
  bool active = true;

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(FeatureRecord, id, label, active)
};

struct PlainPair {
  double first = 0.0;
  double second = 0.0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlainPair, first, second)
}  // namespace macro_compat

class RecordingSax final : public json::json_sax_t {
 public:
  bool null() override {
    events.push_back("null");
    return true;
  }

  bool boolean(bool val) override {
    events.push_back(val ? "bool:true" : "bool:false");
    return true;
  }

  bool number_integer(number_integer_t val) override {
    events.push_back("int:" + std::to_string(val));
    return true;
  }

  bool number_unsigned(number_unsigned_t val) override {
    events.push_back("uint:" + std::to_string(val));
    return true;
  }

  bool number_float(number_float_t val, const string_t& s) override {
    floats.push_back(val);
    events.push_back("float:" + s);
    return true;
  }

  bool string(string_t& val) override {
    events.push_back("str:" + val);
    return true;
  }

  bool binary(binary_t& val) override {
    std::string entry = "bin:" + std::to_string(val.size());
    if (val.has_subtype()) {
      entry += "@sub=" + std::to_string(val.subtype());
    }
    events.push_back(std::move(entry));
    return true;
  }

  bool start_object(std::size_t /*elements*/) override {
    events.push_back("{");
    return true;
  }

  bool key(string_t& val) override {
    events.push_back("key:" + val);
    return true;
  }

  bool end_object() override {
    events.push_back("}");
    return true;
  }

  bool start_array(std::size_t /*elements*/) override {
    events.push_back("[");
    return true;
  }

  bool end_array() override {
    events.push_back("]");
    return true;
  }

  bool parse_error(std::size_t position, const std::string& last_token,
                   const json::exception& ex) override {
    had_error = true;
    error_pos = position;
    error_token = last_token;
    error_id = ex.id;
    return false;
  }

  std::vector<std::string> events;
  std::vector<double> floats;
  bool had_error = false;
  std::size_t error_pos = 0;
  std::string error_token;
  int error_id = 0;
};

TEST(IncludeNlohmannJsonTest, WrapperGuardAndMacroIsolation) {
  EXPECT_TRUE(kHasGeosVendJsonGuard);
  EXPECT_FALSE(kNlohmannMacroLeaked);
  EXPECT_FALSE(kJsonThrowLeaked);
  EXPECT_FALSE(kJsonAssertLeaked);
  EXPECT_FALSE(kJsonHedleyLeaked);
  EXPECT_FALSE(kJsonHasCpp20Leaked);
  EXPECT_FALSE(kJsonUseGlobalUdlsLeaked);

  EXPECT_EQ(NLOHMANN_JSON_VERSION_MAJOR, 3);
  EXPECT_EQ(NLOHMANN_JSON_VERSION_MINOR, 11);
  EXPECT_EQ(NLOHMANN_JSON_VERSION_PATCH, 3);

  const json meta = json::meta();
  EXPECT_EQ(meta.at("name"), "JSON for Modern C++");
  EXPECT_EQ(meta.at("version").at("string"), "3.11.3");
  EXPECT_EQ(meta.at("version").at("major"), 3);
  EXPECT_EQ(meta.at("version").at("minor"), 11);
  EXPECT_EQ(meta.at("version").at("patch"), 3);
  EXPECT_TRUE(meta.contains("platform"));
  EXPECT_TRUE(meta.contains("compiler"));
}

TEST(IncludeNlohmannJsonTest, UserNamespaceNlohmannDoesNotCollide) {
  nlohmann::UnrelatedMarker marker;
  EXPECT_EQ(marker.code, 42);

  json j = {{"status", "ok"}, {"code", marker.code}};
  EXPECT_EQ(j.at("code"), 42);
  EXPECT_FALSE((std::is_same_v<nlohmann::UnrelatedMarker, json>));
}

TEST(IncludeNlohmannJsonTest, ValueTypesAndPredicates) {
  const json j_null;
  EXPECT_TRUE(j_null.is_null());
  EXPECT_TRUE(j_null.is_primitive());
  EXPECT_FALSE(j_null.is_structured());
  EXPECT_EQ(j_null.type(), json::value_t::null);
  EXPECT_STREQ(j_null.type_name(), "null");
  EXPECT_EQ(j_null.dump(), "null");

  const json j_null_ptr(nullptr);
  EXPECT_EQ(j_null, j_null_ptr);

  const json j_bool(true);
  EXPECT_TRUE(j_bool.is_boolean());
  EXPECT_TRUE(j_bool.is_primitive());
  EXPECT_EQ(j_bool.type(), json::value_t::boolean);
  EXPECT_STREQ(j_bool.type_name(), "boolean");
  EXPECT_TRUE(j_bool.get<bool>());

  const auto min_i64 = std::numeric_limits<std::int64_t>::min();
  const json j_int(min_i64);
  EXPECT_TRUE(j_int.is_number());
  EXPECT_TRUE(j_int.is_number_integer());
  EXPECT_FALSE(j_int.is_number_unsigned());
  EXPECT_FALSE(j_int.is_number_float());
  EXPECT_EQ(j_int.type(), json::value_t::number_integer);
  EXPECT_STREQ(j_int.type_name(), "number");
  EXPECT_EQ(j_int.get<std::int64_t>(), min_i64);

  const auto max_u64 = std::numeric_limits<std::uint64_t>::max();
  const json j_uint(max_u64);
  EXPECT_TRUE(j_uint.is_number());
  EXPECT_TRUE(j_uint.is_number_integer());
  EXPECT_TRUE(j_uint.is_number_unsigned());
  EXPECT_FALSE(j_uint.is_number_float());
  EXPECT_EQ(j_uint.type(), json::value_t::number_unsigned);
  EXPECT_EQ(j_uint.get<std::uint64_t>(), max_u64);

  const json j_float(3.5);
  EXPECT_TRUE(j_float.is_number());
  EXPECT_TRUE(j_float.is_number_float());
  EXPECT_FALSE(j_float.is_number_integer());
  EXPECT_EQ(j_float.type(), json::value_t::number_float);
  EXPECT_DOUBLE_EQ(j_float.get<double>(), 3.5);

  const json j_inf(std::numeric_limits<double>::infinity());
  EXPECT_TRUE(j_inf.is_number_float());
  EXPECT_EQ(j_inf.dump(), "null");

  const json j_nan(std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(j_nan.is_number_float());
  EXPECT_TRUE(std::isnan(j_nan.get<double>()));
  EXPECT_EQ(j_nan.dump(), "null");
  EXPECT_FALSE(j_nan == j_nan);
  EXPECT_FALSE(j_nan < json(1));
  EXPECT_FALSE(j_nan > json(1));

  const json j_str("geos");
  EXPECT_TRUE(j_str.is_string());
  EXPECT_STREQ(j_str.type_name(), "string");
  EXPECT_EQ(j_str.get<std::string>(), "geos");

  const json j_arr = {1, 2, 3};
  EXPECT_TRUE(j_arr.is_array());
  EXPECT_TRUE(j_arr.is_structured());
  EXPECT_FALSE(j_arr.is_primitive());
  EXPECT_STREQ(j_arr.type_name(), "array");

  // Two-element array of ["key", val] pairs is deduced as object unless
  // disambiguated with json::array(...) or manual_type.
  const json j_deduced_obj = {{"a", 1}, {"b", 2}};
  EXPECT_TRUE(j_deduced_obj.is_object());
  EXPECT_STREQ(j_deduced_obj.type_name(), "object");

  const json j_forced_arr = json::array({{"a", 1}, {"b", 2}});
  EXPECT_TRUE(j_forced_arr.is_array());
  EXPECT_EQ(j_forced_arr.size(), 2u);

  const json j_manual_arr({{"a", 1}}, false, json::value_t::array);
  EXPECT_TRUE(j_manual_arr.is_array());

  EXPECT_THROW((json({1, 2, 3}, false, json::value_t::object)),
               json::type_error);

  json j_bin =
      json::binary(std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}, 42);
  EXPECT_TRUE(j_bin.is_binary());
  EXPECT_TRUE(j_bin.is_primitive());
  EXPECT_STREQ(j_bin.type_name(), "binary");
  EXPECT_TRUE(j_bin.get_binary().has_subtype());
  EXPECT_EQ(j_bin.get_binary().subtype(), 42u);
  EXPECT_EQ(j_bin.dump(), "{\"bytes\":[222,173,190,239],\"subtype\":42}");
  j_bin.get_binary().set_subtype(7);
  EXPECT_EQ(j_bin.get_binary().subtype(), 7u);
  j_bin.get_binary().clear_subtype();
  EXPECT_FALSE(j_bin.get_binary().has_subtype());
  EXPECT_EQ(j_bin.dump(), "{\"bytes\":[222,173,190,239],\"subtype\":null}");

  const json j_discarded(json::value_t::discarded);
  EXPECT_TRUE(j_discarded.is_discarded());
  EXPECT_STREQ(j_discarded.type_name(), "discarded");
  EXPECT_EQ(j_discarded.dump(), "<discarded>");
  EXPECT_FALSE(j_discarded == j_discarded);
}

TEST(IncludeNlohmannJsonTest, ElementAccessMutationAndIteration) {
  json obj = json::object();
  obj["alpha"] = 10;
  obj["beta"] = "two";
  EXPECT_EQ(obj.size(), 2u);
  EXPECT_FALSE(obj.empty());
  EXPECT_TRUE(obj.contains("alpha"));
  EXPECT_FALSE(obj.contains("gamma"));
  EXPECT_EQ(obj.count("alpha"), 1u);
  EXPECT_EQ(obj.count("gamma"), 0u);
  EXPECT_NE(obj.find("beta"), obj.end());
  EXPECT_EQ(obj.find("gamma"), obj.end());

  EXPECT_EQ(obj.at("alpha"), 10);
  EXPECT_EQ(obj.value("alpha", -1), 10);
  EXPECT_EQ(obj.value("missing_int", 99), 99);
  EXPECT_EQ(obj.value("missing_str", "fallback"), std::string("fallback"));
  EXPECT_EQ(obj.value(json::json_pointer("/beta"), std::string("x")), "two");
  EXPECT_EQ(obj.value(json::json_pointer("/missing/path"), std::string("def")),
            "def");

  // get_ptr / get_to / get_ref / get_binary
  EXPECT_NE(obj.at("alpha").get_ptr<json::number_integer_t*>(), nullptr);
  EXPECT_EQ(*obj.at("alpha").get_ptr<json::number_integer_t*>(), 10);
  EXPECT_EQ(obj.at("alpha").get_ptr<json::string_t*>(), nullptr);
  EXPECT_EQ(obj.at("beta").get_ref<std::string&>(), "two");
  EXPECT_THROW(obj.at("alpha").get_ref<std::string&>(), json::type_error);
  EXPECT_THROW(obj.at("alpha").get_binary(), json::type_error);

  std::string captured_beta;
  obj.at("beta").get_to(captured_beta);
  EXPECT_EQ(captured_beta, "two");

  // emplace / update / erase / structured bindings
  auto [it, inserted] = obj.emplace("gamma", true);
  EXPECT_TRUE(inserted);
  EXPECT_EQ(it.key(), "gamma");
  EXPECT_EQ(it.value(), true);
  EXPECT_FALSE(obj.emplace("alpha", 999).second);

  json base = {{"nested", {{"x", 1}, {"y", 2}}}, {"top", "old"}};
  const json overlay = {{"nested", {{"y", 20}, {"z", 30}}}, {"top", "new"}};
  base.update(overlay, /*merge_objects=*/true);
  EXPECT_EQ(base.at("top"), "new");
  EXPECT_EQ(base.at("nested").at("x"), 1);
  EXPECT_EQ(base.at("nested").at("y"), 20);
  EXPECT_EQ(base.at("nested").at("z"), 30);

  std::vector<std::string> keys;
  for (const auto& [k, v] : obj.items()) {
    keys.push_back(k + "=" + v.dump());
  }
  EXPECT_THAT(keys, ElementsAre("alpha=10", "beta=\"two\"", "gamma=true"));

  EXPECT_EQ(obj.erase("beta"), 1u);
  EXPECT_EQ(obj.erase("missing"), 0u);
  EXPECT_FALSE(obj.contains("beta"));

  // Array access and mutations
  json arr = json::array();
  arr.push_back(1);
  arr += 3;
  arr.emplace_back(4);
  arr.insert(arr.begin() + 1, 2);
  EXPECT_EQ(arr.front(), 1);
  EXPECT_EQ(arr.back(), 4);
  EXPECT_EQ(arr.at(2), 3);
  EXPECT_EQ(arr, (json{1, 2, 3, 4}));

  const json extra = {5, 6};
  arr.insert(arr.end(), extra.begin(), extra.end());
  EXPECT_EQ(arr.size(), 6u);
  arr.erase(std::size_t{1});
  EXPECT_EQ(arr, (json{1, 3, 4, 5, 6}));

  json other = {99};
  arr.swap(other);
  EXPECT_EQ(arr, (json{99}));
  EXPECT_EQ(other.size(), 5u);
  arr.clear();
  EXPECT_TRUE(arr.empty());
}

TEST(IncludeNlohmannJsonTest, OrderedJsonPreservesInsertionOrder) {
  ordered_json oj;
  oj["zeta"] = 1;
  oj["alpha"] = 2;
  oj["mu"] = 3;
  EXPECT_EQ(oj.dump(), "{\"zeta\":1,\"alpha\":2,\"mu\":3}");

  EXPECT_EQ(oj.erase("alpha"), 1u);
  EXPECT_EQ(oj.dump(), "{\"zeta\":1,\"mu\":3}");
  EXPECT_EQ(oj.at("mu"), 3);
  EXPECT_TRUE(oj.contains("zeta"));
  EXPECT_FALSE(oj.contains("alpha"));

  const json regular = {{"zeta", 1}, {"alpha", 2}, {"mu", 3}};
  EXPECT_EQ(regular.dump(), "{\"alpha\":2,\"mu\":3,\"zeta\":1}");
}

TEST(IncludeNlohmannJsonTest, ParsingSerializationCommentsAndStreams) {
  const std::string_view sv = R"({"x": 10, "msg": "café"})";
  const json parsed_sv = json::parse(sv);
  EXPECT_EQ(parsed_sv.at("x"), 10);
  EXPECT_EQ(parsed_sv.at("msg"), "café");

  const json parsed_iter = json::parse(sv.begin(), sv.end());
  EXPECT_EQ(parsed_iter, parsed_sv);

  // ensure_ascii escapes non-ASCII code points including supplementary plane.
  const json emoji = json{{"e", "\xC3\xA9"}, {"s", "\xF0\x9F\x98\x80"}};
  EXPECT_EQ(emoji.dump(-1, ' ', /*ensure_ascii=*/true),
            "{\"e\":\"\\u00e9\",\"s\":\"\\ud83d\\ude00\"}");

  // Comments handling
  const std::string commented = R"({
    // line comment
    "a": 1,
    /* block comment */
    "b": [2, 3]
  })";
  EXPECT_FALSE(json::accept(commented, /*ignore_comments=*/false));
  EXPECT_TRUE(json::accept(commented, /*ignore_comments=*/true));
  const json from_comments =
      json::parse(commented, nullptr, /*allow_exceptions=*/true,
                  /*ignore_comments=*/true);
  EXPECT_EQ(from_comments, (json{{"a", 1}, {"b", {2, 3}}}));

  // Non-throwing parse failure returns discarded
  const json invalid = json::parse("{invalid", nullptr,
                                   /*allow_exceptions=*/false);
  EXPECT_TRUE(invalid.is_discarded());

  // Parser callback filtering
  const json::parser_callback_t keep_only_keep_key =
      [](int depth, json::parse_event_t event, json& parsed) {
        if (depth == 1 && event == json::parse_event_t::key) {
          return parsed == "keep";
        }
        return true;
      };
  const json filtered =
      json::parse(R"({"keep": 1, "drop": 2})", keep_only_keep_key);
  EXPECT_EQ(filtered, (json{{"keep", 1}}));

  // Invalid UTF-8 error handlers
  const json bad_utf8 = json(std::string("ok\xff!"));
  EXPECT_THROW(static_cast<void>(bad_utf8.dump(-1, ' ', false,
                                               json::error_handler_t::strict)),
               json::type_error);
  EXPECT_EQ(bad_utf8.dump(-1, ' ', false, json::error_handler_t::replace),
            "\"ok\xEF\xBF\xBD!\"");
  EXPECT_EQ(bad_utf8.dump(-1, ' ', true, json::error_handler_t::replace),
            "\"ok\\ufffd!\"");
  EXPECT_EQ(bad_utf8.dump(-1, ' ', false, json::error_handler_t::ignore),
            "\"ok!\"");

  // Stream serialization & deserialization, pretty indent, and ADL to_string
  std::ostringstream oss_dotted;
  oss_dotted << std::setw(2) << std::setfill('.') << json{{"k", {1, 2}}};
  const std::string dotted_text = oss_dotted.str();
  EXPECT_EQ(dotted_text, "{\n..\"k\": [\n....1,\n....2\n..]\n}");
  std::istringstream iss_dotted(dotted_text);
  json invalid_round_trip;
  EXPECT_THROW(iss_dotted >> invalid_round_trip, json::parse_error);

  std::ostringstream oss;
  oss << std::setw(2) << json{{"k", {1, 2}}};
  const std::string stream_text = oss.str();
  EXPECT_EQ(stream_text, "{\n  \"k\": [\n    1,\n    2\n  ]\n}");

  std::istringstream iss(stream_text);
  json round_trip;
  iss >> round_trip;
  EXPECT_EQ(round_trip, (json{{"k", {1, 2}}}));
  EXPECT_EQ(to_string(round_trip), "{\"k\":[1,2]}");
  EXPECT_EQ(geos_nlohmann::to_string(round_trip), "{\"k\":[1,2]}");
}

TEST(IncludeNlohmannJsonTest, SaxParsing) {
  RecordingSax sax;
  const std::string input =
      R"({"n":null,"b":true,"i":-5,"u":9,"a":[1.5,"hi"]})";
  EXPECT_TRUE(json::sax_parse(input, &sax));
  EXPECT_FALSE(sax.had_error);
  EXPECT_THAT(sax.events,
              ElementsAre("{", "key:n", "null", "key:b", "bool:true", "key:i",
                          "int:-5", "key:u", "uint:9", "key:a", "[",
                          "float:1.5", "str:hi", "]", "}"));
  EXPECT_THAT(sax.floats, ElementsAre(1.5));

  // SAX binary event via CBOR (untagged) and MessagePack (with subtype)
  RecordingSax cbor_sax;
  const std::vector<std::uint8_t> cbor =
      json::to_cbor(json::binary(std::vector<std::uint8_t>{1, 2, 3}));
  EXPECT_TRUE(json::sax_parse(cbor, &cbor_sax, json::input_format_t::cbor));
  EXPECT_THAT(cbor_sax.events, ElementsAre("bin:3"));

  RecordingSax cbor_tagged_sax;
  const std::vector<std::uint8_t> cbor_tagged =
      json::to_cbor(json::binary(std::vector<std::uint8_t>{1, 2, 3}, 15));
  EXPECT_FALSE(json::sax_parse(cbor_tagged, &cbor_tagged_sax,
                               json::input_format_t::cbor));
  EXPECT_TRUE(cbor_tagged_sax.had_error);
  EXPECT_EQ(cbor_tagged_sax.error_id, 112);

  RecordingSax msgpack_sax;
  const std::vector<std::uint8_t> msgpack_bin =
      json::to_msgpack(json::binary(std::vector<std::uint8_t>{1, 2, 3}, 15));
  EXPECT_TRUE(json::sax_parse(msgpack_bin, &msgpack_sax,
                              json::input_format_t::msgpack));
  EXPECT_THAT(msgpack_sax.events, ElementsAre("bin:3@sub=15"));

  // SAX parse_error path
  RecordingSax err_sax;
  EXPECT_FALSE(json::sax_parse("{\"a\": }", &err_sax));
  EXPECT_TRUE(err_sax.had_error);
  EXPECT_EQ(err_sax.error_id, 101);
  EXPECT_GT(err_sax.error_pos, 0u);
}

TEST(IncludeNlohmannJsonTest, JsonLiteralsPointerFlattenAndPatch) {
  // Global UDLs and explicit namespace literals
  const json doc =
      R"({"items": [{"a/b": 1, "c~d": [10, 20]}], "flag": true})"_json;
  EXPECT_TRUE(doc.is_object());
  constexpr std::string_view kLiteral = "{\"v\":42}";
  const json via_literal_ns =
      ::geos_nlohmann::literals::json_literals::operator""_json(
          kLiteral.data(), kLiteral.size());
  EXPECT_EQ(via_literal_ns.at("v"), 42);

  constexpr std::string_view kPtrLiteral = "/items/0";
  const json::json_pointer via_ptr_literal_ns =
      ::geos_nlohmann::literals::json_literals::operator""_json_pointer(
          kPtrLiteral.data(), kPtrLiteral.size());
  EXPECT_EQ(via_ptr_literal_ns.to_string(), "/items/0");

  const auto root_ptr = ""_json_pointer;
  EXPECT_TRUE(root_ptr.empty());
  EXPECT_EQ(root_ptr.to_string(), "");

  auto ptr = root_ptr / "items" / std::size_t{0};
  ptr.push_back("a/b");
  EXPECT_EQ(ptr.to_string(), "/items/0/a~1b");
  EXPECT_EQ(ptr.back(), "a/b");
  EXPECT_EQ(doc.at(ptr), 1);

  ptr.pop_back();
  ptr /= "c~d";
  ptr /= std::size_t{1};
  EXPECT_EQ(ptr.to_string(), "/items/0/c~0d/1");
  EXPECT_EQ(doc.at(ptr), 20);
  EXPECT_EQ(ptr.parent_pointer(), json::json_pointer("/items/0/c~0d"));
  EXPECT_TRUE(doc.contains(ptr));
  EXPECT_FALSE(doc.contains(json::json_pointer("/items/0/c~0d/9")));

  EXPECT_THROW((json::json_pointer("/bad~2escape")), json::parse_error);
  EXPECT_THROW(doc.at(json::json_pointer("/items/-")), json::out_of_range);

  // Flatten and unflatten round-trip (note empty containers flatten to null)
  const json nested = {
      {"pi", 3.141},
      {"slash/tilde~key", {1, 2}},
      {"empty_obj", json::object()},
  };
  const json flat = nested.flatten();
  EXPECT_EQ(flat.at("/pi"), 3.141);
  EXPECT_EQ(flat.at("/slash~1tilde~0key/0"), 1);
  EXPECT_EQ(flat.at("/slash~1tilde~0key/1"), 2);
  EXPECT_TRUE(flat.at("/empty_obj").is_null());

  const json unflattened = flat.unflatten();
  EXPECT_EQ(unflattened.at("pi"), 3.141);
  EXPECT_EQ(unflattened.at("slash/tilde~key"), (json{1, 2}));
  EXPECT_TRUE(unflattened.at("empty_obj").is_null());

  EXPECT_THROW(json({{"/a", 1}, {"/a/b", 2}}).unflatten(), json::type_error);

  // RFC 6902 diff / patch / patch_inplace
  const json target =
      R"({"items": [{"a/b": 99, "c~d": [10]}], "extra": "v"})"_json;
  const json patch_ops = json::diff(doc, target);
  EXPECT_EQ(doc.patch(patch_ops), target);

  json patched = doc;
  const json explicit_patch = R"([
    {"op": "test", "path": "/flag", "value": true},
    {"op": "move", "from": "/flag", "path": "/moved_flag"},
    {"op": "copy", "from": "/items/0/a~1b", "path": "/copied_ab"},
    {"op": "add", "path": "/items/0/c~0d/-", "value": 30},
    {"op": "remove", "path": "/items/0/c~0d/0"}
  ])"_json;
  patched.patch_inplace(explicit_patch);
  EXPECT_FALSE(patched.contains("flag"));
  EXPECT_EQ(patched.at("moved_flag"), true);
  EXPECT_EQ(patched.at("copied_ab"), 1);
  EXPECT_EQ(patched.at(json::json_pointer("/items/0/c~0d")), (json{20, 30}));

  EXPECT_THROW(
      doc.patch(R"([{"op": "test", "path": "/flag", "value": false}])"_json),
      json::other_error);

  // RFC 7396 merge_patch
  json merged = R"({"title": "A", "meta": {"x": 1, "y": 2}, "drop": 9})"_json;
  merged.merge_patch(R"({"meta": {"y": null, "z": 3}, "drop": null})"_json);
  EXPECT_EQ(merged, R"({"title": "A", "meta": {"x": 1, "z": 3}})"_json);
}

TEST(IncludeNlohmannJsonTest, BinaryFormatsRoundTrip) {
  const json sample = {
      {"int", -7},
      {"uint", 42u},
      {"flt", 2.5},
      {"flag", true},
      {"nul", nullptr},
      {"text", "geos"},
      {"arr", {1, "two", false}},
  };

  // CBOR
  const std::vector<std::uint8_t> cbor = json::to_cbor(sample);
  EXPECT_EQ(json::from_cbor(cbor), sample);

  const json bin_doc =
      json{{"payload",
            json::binary(std::vector<std::uint8_t>{0x01, 0x02, 0x03}, 99)}};
  const std::vector<std::uint8_t> bin_cbor = json::to_cbor(bin_doc);
  EXPECT_THROW(static_cast<void>(json::from_cbor(
                   bin_cbor, true, true, json::cbor_tag_handler_t::error)),
               json::parse_error);
  const json cbor_ignored =
      json::from_cbor(bin_cbor, true, true, json::cbor_tag_handler_t::ignore);
  EXPECT_FALSE(cbor_ignored.at("payload").get_binary().has_subtype());
  const json cbor_stored =
      json::from_cbor(bin_cbor, true, true, json::cbor_tag_handler_t::store);
  EXPECT_TRUE(cbor_stored.at("payload").get_binary().has_subtype());
  EXPECT_EQ(cbor_stored.at("payload").get_binary().subtype(), 99u);
  EXPECT_EQ(cbor_stored, bin_doc);

  // MessagePack
  const std::vector<std::uint8_t> msgpack = json::to_msgpack(sample);
  EXPECT_EQ(json::from_msgpack(msgpack.begin(), msgpack.end()), sample);

  // UBJSON (typed and sized)
  const std::vector<std::uint8_t> ubjson =
      json::to_ubjson(sample, /*use_size=*/true, /*use_type=*/true);
  EXPECT_EQ(json::from_ubjson(ubjson), sample);

  // BJData
  const std::vector<std::uint8_t> bjdata =
      json::to_bjdata(sample, /*use_size=*/true, /*use_type=*/true);
  EXPECT_EQ(json::from_bjdata(bjdata), sample);

  // BSON (top-level value must be object or binary)
  const std::vector<std::uint8_t> bson = json::to_bson(sample);
  EXPECT_EQ(json::from_bson(bson), sample);
  EXPECT_THROW(static_cast<void>(json::to_bson(json{1, 2, 3})),
               json::type_error);
}

TEST(IncludeNlohmannJsonTest, HashingAndValueTypeOrdering) {
  std::hash<json> hasher;
  const std::size_t h_null = hasher(json(nullptr));
  const std::size_t h_false = hasher(json(false));
  const std::size_t h_int = hasher(json(0));
  const std::size_t h_uint = hasher(json(0u));
  const std::size_t h_dbl = hasher(json(0.0));
  EXPECT_NE(h_null, h_false);
  EXPECT_NE(h_int, h_uint);
  EXPECT_NE(h_int, h_dbl);

  std::unordered_map<json, std::string> lookup;
  lookup[nullptr] = "null";
  lookup[true] = "bool";
  lookup[42] = "num";
  lookup["key"] = "str";
  lookup[json::array({1, 2})] = "arr";
  lookup[json{{"a", 1}}] = "obj";
  EXPECT_EQ(lookup.at(nullptr), "null");
  EXPECT_EQ(lookup.at(42), "num");
  EXPECT_EQ(lookup.at(json::array({1, 2})), "arr");

  // std::less<value_t> follows nlohmann ordering:
  // null < boolean < number < object < array < string < binary
  const std::less<json::value_t> vt_less;
  EXPECT_TRUE(vt_less(json::value_t::null, json::value_t::boolean));
  EXPECT_TRUE(vt_less(json::value_t::boolean, json::value_t::number_integer));
  EXPECT_TRUE(vt_less(json::value_t::number_float, json::value_t::object));
  EXPECT_TRUE(vt_less(json::value_t::object, json::value_t::array));
  EXPECT_TRUE(vt_less(json::value_t::array, json::value_t::string));
  EXPECT_TRUE(vt_less(json::value_t::string, json::value_t::binary));
  EXPECT_FALSE(
      vt_less(json::value_t::number_integer, json::value_t::number_float));
}

TEST(IncludeNlohmannJsonTest, AdlMacrosAndStlContainers) {
  // Custom ADL to_json / from_json using geos_nlohmann::json directly
  const Point2D pt{1.25, -4.5};
  const json j_pt = pt;
  EXPECT_EQ(j_pt.at("x"), 1.25);
  EXPECT_EQ(j_pt.at("y"), -4.5);
  EXPECT_EQ(j_pt.get<Point2D>(), pt);

  // Enum serialization (templated on BasicJsonType, works on json &
  // ordered_json)
  const json j_enum = GeometryKind::kLineString;
  EXPECT_EQ(j_enum, "LineString");
  EXPECT_EQ(j_enum.get<GeometryKind>(), GeometryKind::kLineString);
  EXPECT_EQ(json("unrecognized").get<GeometryKind>(), GeometryKind::kUnknown);

  const ordered_json oj_enum = GeometryKind::kPolygon;
  EXPECT_EQ(oj_enum, "Polygon");
  EXPECT_EQ(oj_enum.get<GeometryKind>(), GeometryKind::kPolygon);

  // Local namespace alias enables NLOHMANN_DEFINE_TYPE_* macros
  const macro_compat::FeatureRecord rec{7, "lake", false};
  const json j_rec = rec;
  EXPECT_EQ(j_rec, (json{{"id", 7}, {"label", "lake"}, {"active", false}}));
  const auto defaulted = json{{"id", 11}}.get<macro_compat::FeatureRecord>();
  EXPECT_EQ(defaulted.id, 11);
  EXPECT_EQ(defaulted.label, "default");
  EXPECT_TRUE(defaulted.active);

  const macro_compat::PlainPair pair_in{2.5, 9.5};
  const json j_pair_struct = pair_in;
  const auto pair_out = j_pair_struct.get<macro_compat::PlainPair>();
  EXPECT_DOUBLE_EQ(pair_out.first, 2.5);
  EXPECT_DOUBLE_EQ(pair_out.second, 9.5);

  // Standard library containers round-trip
  const std::vector<bool> bools = {true, false, true};
  EXPECT_EQ(json(bools).get<std::vector<bool>>(), bools);

  const std::array<int, 3> arr3 = {4, 5, 6};
  EXPECT_EQ((json(arr3).get<std::array<int, 3>>()), arr3);

  const std::forward_list<int> flist = {10, 20, 30};
  EXPECT_EQ(json(flist).get<std::forward_list<int>>(), flist);

  const std::valarray<int> val_in = {7, 8, 9};
  const auto val_out = json(val_in).get<std::valarray<int>>();
  ASSERT_EQ(val_out.size(), 3u);
  EXPECT_EQ(val_out[0], 7);
  EXPECT_EQ(val_out[1], 8);
  EXPECT_EQ(val_out[2], 9);

  const std::pair<std::string, double> std_pair = {"elev", 123.5};
  EXPECT_EQ((json(std_pair).get<std::pair<std::string, double>>()), std_pair);

  const std::tuple<int, std::string, bool> tup = {5, "ok", true};
  EXPECT_EQ((json(tup).get<std::tuple<int, std::string, bool>>()), tup);

  const std::map<int, std::string> int_map = {{1, "one"}, {2, "two"}};
  const json j_int_map = int_map;
  EXPECT_TRUE(j_int_map.is_array());
  EXPECT_EQ((j_int_map.get<std::map<int, std::string>>()), int_map);
}

TEST(IncludeNlohmannJsonTest, ExceptionHierarchyAndDiagnostics) {
  try {
    static_cast<void>(json::parse("[1, 2, ]"));
    FAIL() << "Expected json::parse_error";
  } catch (const json::parse_error& ex) {
    const json::exception& base = ex;
    EXPECT_EQ(base.id, 101);
    EXPECT_GT(ex.byte, 0u);
    EXPECT_NE(std::string(base.what()).find("parse error"), std::string::npos);
  }

  try {
    static_cast<void>(json("text").get<int>());
    FAIL() << "Expected json::type_error";
  } catch (const json::type_error& ex) {
    EXPECT_EQ(ex.id, 302);
  }

  try {
    const json arr = {1, 2};
    static_cast<void>(arr.at(5));
    FAIL() << "Expected json::out_of_range";
  } catch (const json::out_of_range& ex) {
    EXPECT_EQ(ex.id, 401);
  }

  try {
    const json obj = {{"a", 1}};
    static_cast<void>(obj.at("missing"));
    FAIL() << "Expected json::out_of_range";
  } catch (const json::out_of_range& ex) {
    EXPECT_EQ(ex.id, 403);
  }

  try {
    json a = {1, 2};
    const json b = {3, 4};
    a.erase(b.begin());
    FAIL() << "Expected json::invalid_iterator";
  } catch (const json::invalid_iterator& ex) {
    EXPECT_EQ(ex.id, 202);
  }
}

}  // namespace
}  // namespace geos_google
