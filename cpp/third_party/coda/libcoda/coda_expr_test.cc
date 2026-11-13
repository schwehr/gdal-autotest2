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
// Tests CODA expression parsing, metadata, printing, and evaluation
// (libcoda/coda-expr.c, libcoda/coda-expr-parser.y,
// libcoda/coda-expr-tokenizer.l).
//
// google3 build caveats:
//   - CODA_MAX_RECURSION_DEPTH is set to 100 in third_party/stcorp_coda/BUILD.
//   - CODA's built-in leap second table ends at 2017-01-01; time/strtime tests
//     here do not use UTC leap-second conversions.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-parser.y
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-tokenizer.l

#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-expr.h"
}  // extern "C"

#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

thread_local std::string* g_print_buffer = nullptr;

int CapturePrint(const char* fmt, ...) {
  if (g_print_buffer == nullptr) {
    return -1;
  }
  va_list ap;
  va_start(ap, fmt);
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int needed = std::vsnprintf(nullptr, 0, fmt, ap_copy);
  va_end(ap_copy);
  if (needed < 0) {
    va_end(ap);
    return -1;
  }
  std::string chunk(static_cast<size_t>(needed) + 1, '\0');
  std::vsnprintf(chunk.data(), chunk.size(), fmt, ap);
  va_end(ap);
  chunk.resize(static_cast<size_t>(needed));
  g_print_buffer->append(chunk);
  return needed;
}

std::string PrintWith(const coda_expression* expr,
                      int (*print_fn)(const coda_expression*,
                                      int (*)(const char*, ...))) {
  std::string out;
  g_print_buffer = &out;
  const absl::Cleanup clear_buf = [] { g_print_buffer = nullptr; };
  EXPECT_EQ(CODA_SUCCESS, print_fn(expr, &CapturePrint));
  return out;
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendLeDouble(std::vector<uint8_t>& out, double v) {
  AppendLe64(out, std::bit_cast<uint64_t>(v));
}

void AppendText(std::vector<uint8_t>& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
}

constexpr std::string_view kIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="ExprTestClass">
  <cd:ProductType name="ExprBin">
    <cd:ProductDefinition id="expr_bin" format="binary" version="3">
      <cd:DetectionRule>
        <cd:MatchData offset="0">EXPRBIN1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="ExprAsc">
    <cd:ProductDefinition id="expr_asc" format="ascii" version="2">
      <cd:DetectionRule>
        <cd:MatchData offset="0">EXPRASC1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

constexpr std::string_view kBinProductXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="expr_bin" format="binary" version="3">
  <cd:Record format="binary">
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="i32">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:BitSize>32</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u64">
      <cd:Integer>
        <cd:NativeType>uint64</cd:NativeType>
        <cd:BitSize>64</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="f64">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:BitSize>64</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Float>
    </cd:Field>
    <cd:Field name="txt">
      <cd:Text>
        <cd:ByteSize>6</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="empty_txt">
      <cd:Text>
        <cd:ByteSize>0</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="grp_name">
      <cd:Text>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="raw5">
      <cd:Raw>
        <cd:BitSize>5</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="pad3">
      <cd:Raw>
        <cd:BitSize>3</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>4</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:BitSize>32</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="mat2d">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
          <cd:BitSize>64</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="str_arr">
      <cd:Array>
        <cd:Dimension>3</cd:Dimension>
        <cd:Text>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Text>
      </cd:Array>
    </cd:Field>
    <cd:Field name="empty_arr">
      <cd:Array>
        <cd:Dimension>0</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:BitSize>32</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="tstamp">
      <cd:Time timeformat="binary_envisat_datetime"/>
    </cd:Field>
    <cd:Field name="true">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="opt_missing">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:BitSize>32</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
      <cd:Optional/>
      <cd:Available>false</cd:Available>
    </cd:Field>
  </cd:Record>
  <cd:ProductVariable name="scalar_var">
    <cd:Init>$scalar_var = 99</cd:Init>
  </cd:ProductVariable>
  <cd:ProductVariable name="arr_var">
    <cd:Dimension>4</cd:Dimension>
    <cd:Init>for i = 0 to 3 do $arr_var[i] = (i + 1) * 10</cd:Init>
  </cd:ProductVariable>
</cd:ProductDefinition>
)";

constexpr std::string_view kAscProductXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="expr_asc" format="ascii" version="2">
  <cd:Record format="ascii">
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

std::vector<uint8_t> BuildBinaryProductBytes() {
  std::vector<uint8_t> data;
  // 1. magic (8 bytes, offset 0..7)
  AppendText(data, "EXPRBIN1");
  // 2. i32 = -123456789 (4 bytes, offset 8..11)
  AppendLe32(data, static_cast<uint32_t>(int32_t{-123456789}));
  // 3. u64 = 0x0102030405060708ULL (8 bytes, offset 12..19)
  AppendLe64(data, 0x0102030405060708ULL);
  // 4. f64 = 3.5 (8 bytes, offset 20..27)
  AppendLeDouble(data, 3.5);
  // 5. txt = "abcdef" (6 bytes, offset 28..33)
  AppendText(data, "abcdef");
  // 6. empty_txt (0 bytes)
  // 7. grp_name = "grp" (3 bytes, offset 34..36)
  AppendText(data, "grp");
  // 8-9. raw5 (5 bits: 0x15) + pad3 (3 bits: 0) = 0xA8 (1 byte, offset 37)
  data.push_back(0xA8);
  // 10. arr[4] = {10, 20, 30, 40} (16 bytes, offset 38..53)
  for (const int32_t v : {10, 20, 30, 40}) {
    AppendLe32(data, static_cast<uint32_t>(v));
  }
  // 11. mat2d[2][3] (48 bytes, offset 54..101)
  for (const double v : {1.5, -2.5, 4.0, 0.5, 9.5, -1.0}) {
    AppendLeDouble(data, v);
  }
  // 12. str_arr[3] (12 bytes, offset 102..113)
  AppendText(data, "beta");
  AppendText(data, "alfa");
  AppendText(data, "gama");
  // 13. empty_arr (0 bytes)
  // 14. tstamp: days=1, seconds=2, microseconds=3 (BE, 12 bytes, offset
  // 114..125)
  AppendBe32(data, 1);
  AppendBe32(data, 2);
  AppendBe32(data, 3);
  // 15. true = 7 (1 byte, offset 126)
  data.push_back(7);
  return data;
}

std::vector<uint8_t> BuildTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 2> kEntries =
      {{
          {"products/expr_bin.xml", kBinProductXml},
          {"products/expr_asc.xml", kAscProductXml},
      }};
  return BuildCodadefBytes(kIndexXml, kEntries);
}

void ExpectBoolExpr(const char* expr_str, int expected,
                    const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_expression_type type;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &type));
  ASSERT_EQ(coda_expression_boolean, type) << expr_str;
  int actual = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_eval_bool(expr.get(), cursor, &actual))
      << expr_str;
  EXPECT_EQ(expected, actual) << expr_str;
}

void ExpectBoolError(const char* expr_str, int expected_errno,
                     const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_set_error(CODA_SUCCESS, nullptr);
  int actual = -1;
  EXPECT_EQ(-1, coda_expression_eval_bool(expr.get(), cursor, &actual))
      << expr_str;
  EXPECT_EQ(expected_errno, coda_errno) << expr_str;
}

void ExpectIntExpr(const char* expr_str, int64_t expected,
                   const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_expression_type type;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &type));
  ASSERT_EQ(coda_expression_integer, type) << expr_str;
  int64_t actual = -999999;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_eval_integer(expr.get(), cursor, &actual))
      << expr_str;
  EXPECT_EQ(expected, actual) << expr_str;
}

void ExpectIntError(const char* expr_str, int expected_errno,
                    const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_set_error(CODA_SUCCESS, nullptr);
  int64_t actual = 0;
  EXPECT_EQ(-1, coda_expression_eval_integer(expr.get(), cursor, &actual))
      << expr_str;
  EXPECT_EQ(expected_errno, coda_errno) << expr_str;
}

void ExpectFloatExpr(const char* expr_str, double expected,
                     const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_expression_type type;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &type));
  ASSERT_EQ(coda_expression_float, type) << expr_str;
  double actual = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_eval_float(expr.get(), cursor, &actual))
      << expr_str;
  EXPECT_DOUBLE_EQ(expected, actual) << expr_str;
}

void ExpectFloatError(const char* expr_str, int expected_errno,
                      const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_set_error(CODA_SUCCESS, nullptr);
  double actual = 0.0;
  EXPECT_EQ(-1, coda_expression_eval_float(expr.get(), cursor, &actual))
      << expr_str;
  EXPECT_EQ(expected_errno, coda_errno) << expr_str;
}

void ExpectStringExpr(const char* expr_str, std::string_view expected,
                      const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_expression_type type;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &type));
  ASSERT_EQ(coda_expression_string, type) << expr_str;
  ScopedCodaString actual;
  long length = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_string(
                              expr.get(), cursor, actual.OutParam(), &length))
      << expr_str;
  EXPECT_EQ(static_cast<long>(expected.size()), length) << expr_str;
  if (expected.empty()) {
    EXPECT_EQ(nullptr, actual.get()) << expr_str;
  } else {
    ASSERT_NE(nullptr, actual.get()) << expr_str;
    EXPECT_EQ(expected,
              std::string_view(actual.get(), static_cast<size_t>(length)))
        << expr_str;
  }
}

void ExpectStringError(const char* expr_str, int expected_errno,
                       const coda_cursor* cursor = nullptr) {
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(expr_str, expr.OutParam()))
      << expr_str;
  coda_set_error(CODA_SUCCESS, nullptr);
  // Use raw char* rather than ScopedCodaString because eval_string(expr_bytes)
  // frees *value on coda_cursor_read_bits error without nulling *value
  // (coda-expr.c:3050), which would otherwise double-free on destruction.
  char* value = nullptr;
  long length = -1;
  const int rc =
      coda_expression_eval_string(expr.get(), cursor, &value, &length);
  EXPECT_EQ(-1, rc) << expr_str;
  if (rc == CODA_SUCCESS && value != nullptr) {
    coda_free(value);
  }
  EXPECT_EQ(expected_errno, coda_errno) << expr_str;
}

TEST(CodaExprTest, GetTypeNameReturnsExpectedStringsAndUnknownForInvalidEnum) {
  struct TypeNameCase {
    coda_expression_type type;
    std::string_view expected;
  };
  constexpr TypeNameCase kCases[] = {
      {.type = coda_expression_boolean, .expected = "boolean"},
      {.type = coda_expression_integer, .expected = "integer"},
      {.type = coda_expression_float, .expected = "float"},
      {.type = coda_expression_string, .expected = "string"},
      {.type = coda_expression_node, .expected = "node"},
      {.type = coda_expression_void, .expected = "void"},
      // Value 6 stays within [0, 7] for enum coda_expression_type_enum
      // (avoiding UBSan -fsanitize=enum failure) while hitting default:
      // "unknown".
      {.type = static_cast<coda_expression_type>(6), .expected = "unknown"},
  };

  for (const TypeNameCase& tc : kCases) {
    const char* name = coda_expression_get_type_name(tc.type);
    ASSERT_NE(nullptr, name);
    EXPECT_EQ(tc.expected, std::string_view(name));
  }
}

TEST(CodaExprTest, FromStringHandlesInvalidArgsEscapesAndMaxRecursionDepth) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedExpr expr;
  EXPECT_EQ(-1, coda_expression_from_string(nullptr, expr.OutParam()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_from_string("1", nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  constexpr const char* kSyntaxErrors[] = {"", "1 + ", "no_such_func(1)"};
  for (const char* bad : kSyntaxErrors) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_expression_from_string(bad, expr.OutParam())) << bad;
    EXPECT_EQ(nullptr, expr.get());
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno) << bad;
  }

  // Integer overflow is tokenized as INT_VALUE and rejected by
  // integer_constant_new().
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_from_string("99999999999999999999999999999",
                                            expr.OutParam()));
  EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);

  // Invalid float string passed directly to coda_expression_new().
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_expression_new(expr_constant_float, strdup("not_a_float"),
                                nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);

  // decode_escaped_string(NULL) produces a 0-length string constant.
  ScopedExpr null_str(coda_expression_new(expr_constant_string, nullptr,
                                          nullptr, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, null_str.get());
  ScopedCodaString out;
  long len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_string(null_str.get(), nullptr,
                                                      out.OutParam(), &len));
  EXPECT_EQ(0, len);
  EXPECT_EQ(nullptr, out.get());

  // Invalid escape sequences in string_constant_new() /
  // decode_escaped_string().
  constexpr const char* kBadEscapes[] = {
      R"("\q")",
      R"("\1q")",
      R"("\12q")",
  };
  for (const char* bad_escape : kBadEscapes) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_expression_from_string(bad_escape, expr.OutParam()))
        << bad_escape;
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno) << bad_escape;
  }

  // Nesting past CODA_MAX_RECURSION_DEPTH (100 in google3) fails cleanly.
  std::string deep = "1";
  for (int i = 0; i < 105; ++i) {
    deep = "1+(" + deep + ")";
  }
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_from_string(deep.c_str(), expr.OutParam()));
  EXPECT_EQ(nullptr, expr.get());
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("maximum recursion depth"));
}

TEST(CodaExprTest, MetadataGetTypeIsConstantAndIsEqual) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  coda_expression_type type = coda_expression_void;
  EXPECT_EQ(-1, coda_expression_get_type(nullptr, &type));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_is_constant(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  struct MetaCase {
    const char* expr;
    coda_expression_type expected_type;
    int expected_const;
  };
  constexpr MetaCase kMetaCases[] = {
      {.expr = "true",
       .expected_type = coda_expression_boolean,
       .expected_const = 1},
      {.expr = "42",
       .expected_type = coda_expression_integer,
       .expected_const = 1},
      {.expr = "1.5",
       .expected_type = coda_expression_float,
       .expected_const = 1},
      {.expr = "\"hi\"",
       .expected_type = coda_expression_string,
       .expected_const = 1},
      {.expr = "r\"hi\"",
       .expected_type = coda_expression_string,
       .expected_const = 1},
      {.expr = "/a",
       .expected_type = coda_expression_node,
       .expected_const = 0},
      {.expr = "goto(.)",
       .expected_type = coda_expression_void,
       .expected_const = 0},
      {.expr = "filesize()",
       .expected_type = coda_expression_integer,
       .expected_const = 0},
      {.expr = "filename()",
       .expected_type = coda_expression_string,
       .expected_const = 0},
      {.expr = "productclass()",
       .expected_type = coda_expression_string,
       .expected_const = 0},
      {.expr = "productformat()",
       .expected_type = coda_expression_string,
       .expected_const = 0},
      {.expr = "producttype()",
       .expected_type = coda_expression_string,
       .expected_const = 0},
      {.expr = "productversion()",
       .expected_type = coda_expression_integer,
       .expected_const = 0},
      {.expr = "$v",
       .expected_type = coda_expression_integer,
       .expected_const = 0},
      {.expr = "$v = 1",
       .expected_type = coda_expression_void,
       .expected_const = 0},
      {.expr = "index($v, $v == 1)",
       .expected_type = coda_expression_integer,
       .expected_const = 0},
      {.expr = "exists($v, $v == 1)",
       .expected_type = coda_expression_boolean,
       .expected_const = 0},
  };
  for (const MetaCase& tc : kMetaCases) {
    ScopedExpr e;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(tc.expr, e.OutParam()))
        << tc.expr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(e.get(), &type));
    EXPECT_EQ(tc.expected_type, type) << tc.expr;
    EXPECT_EQ(tc.expected_const, coda_expression_is_constant(e.get()))
        << tc.expr;
  }

  ScopedExpr b_true, b_false, f1, f2, i1, i2, s1, s2, s3, rs1;
  ScopedExpr op_a, op_b, op_c, path_a, path_b, path_idx;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("true", b_true.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("false", b_false.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("1.5", f1.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("2.5", f2.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("7", i1.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("8", i2.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("\"ab\"", s1.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("\"ac\"", s2.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("\"abc\"", s3.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("r\"ab\"", rs1.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("1 + 2", op_a.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("1 + 2", op_b.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("2 + 1", op_c.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("/a", path_a.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("/b", path_b.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("/{0}", path_idx.OutParam()));

  EXPECT_EQ(1, coda_expression_is_equal(nullptr, nullptr));
  EXPECT_EQ(0, coda_expression_is_equal(nullptr, i1.get()));
  EXPECT_EQ(0, coda_expression_is_equal(i1.get(), nullptr));
  EXPECT_EQ(0, coda_expression_is_equal(i1.get(), f1.get()));
  EXPECT_EQ(1, coda_expression_is_equal(b_true.get(), b_true.get()));
  EXPECT_EQ(0, coda_expression_is_equal(b_true.get(), b_false.get()));
  EXPECT_EQ(1, coda_expression_is_equal(f1.get(), f1.get()));
  EXPECT_EQ(0, coda_expression_is_equal(f1.get(), f2.get()));
  EXPECT_EQ(1, coda_expression_is_equal(i1.get(), i1.get()));
  EXPECT_EQ(0, coda_expression_is_equal(i1.get(), i2.get()));
  EXPECT_EQ(1, coda_expression_is_equal(s1.get(), s1.get()));
  EXPECT_EQ(0, coda_expression_is_equal(s1.get(), s2.get()));
  EXPECT_EQ(0, coda_expression_is_equal(s1.get(), s3.get()));
  EXPECT_EQ(0, coda_expression_is_equal(s1.get(), rs1.get()));
  EXPECT_EQ(1, coda_expression_is_equal(op_a.get(), op_b.get()));
  EXPECT_EQ(0, coda_expression_is_equal(op_a.get(), op_c.get()));
  EXPECT_EQ(1, coda_expression_is_equal(path_a.get(), path_a.get()));
  EXPECT_EQ(0, coda_expression_is_equal(path_a.get(), path_b.get()));
  EXPECT_EQ(0, coda_expression_is_equal(path_a.get(), path_idx.get()));
  EXPECT_EQ(0, coda_expression_is_equal(path_idx.get(), path_a.get()));
}

TEST(CodaExprTest, EvalRejectsWrongResultTypesAndNullCursors) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedExpr b_const, i_const, f_const, s_const, n_expr, v_expr;
  ScopedExpr b_nonconst, i_nonconst, f_nonconst, s_nonconst;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("true", b_const.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("1", i_const.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("1.0", f_const.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("\"x\"", s_const.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("/a", n_expr.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("goto(.)", v_expr.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("exists(/a)", b_nonconst.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("filesize()", i_nonconst.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("float(/a)", f_nonconst.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("filename()", s_nonconst.OutParam()));

  int b_out = 0;
  int64_t i_out = 0;
  double f_out = 0.0;
  ScopedCodaString s_out;
  long s_len = 0;
  coda_cursor dummy_cursor{};

  // Type mismatches.
  EXPECT_EQ(-1, coda_expression_eval_bool(i_const.get(), nullptr, &b_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_integer(f_const.get(), nullptr, &i_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_float(s_const.get(), nullptr, &f_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_string(b_const.get(), nullptr,
                                            s_out.OutParam(), &s_len));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_node(i_const.get(), &dummy_cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_void(i_const.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // NULL cursor on non-constant expressions.
  EXPECT_EQ(-1, coda_expression_eval_bool(b_nonconst.get(), nullptr, &b_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1,
            coda_expression_eval_integer(i_nonconst.get(), nullptr, &i_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_float(f_nonconst.get(), nullptr, &f_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_string(s_nonconst.get(), nullptr,
                                            s_out.OutParam(), &s_len));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_node(n_expr.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_expression_eval_void(v_expr.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST(CodaExprTest, ConstantBooleanComparisonsLogicalAndFloatPredicates) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct BoolCase {
    const char* expr;
    int expected;
  };
  constexpr BoolCase kCases[] = {
      {"true", 1},
      {"false", 0},
      {"!true", 0},
      {"!false", 1},
      {"true && true", 1},
      {"true && false", 0},
      {"false && (1 / 0 == 0)", 0},
      {"false || false", 0},
      {"false || true", 1},
      {"true || (1 / 0 == 0)", 1},
      // Integer comparisons
      {"3 == 3", 1},
      {"3 == 4", 0},
      {"3 != 4", 1},
      {"3 != 3", 0},
      {"4 > 3", 1},
      {"3 > 4", 0},
      {"3 >= 3", 1},
      {"2 >= 3", 0},
      {"2 < 3", 1},
      {"3 < 2", 0},
      {"3 <= 3", 1},
      {"4 <= 3", 0},
      // Float and mixed float/int comparisons
      {"1.5 == 1.5", 1},
      {"1.0 == 1", 1},
      {"1 == 1.5", 0},
      {"1.5 != 2", 1},
      {"2.5 > 2", 1},
      {"2 > 2.5", 0},
      {"2.0 >= 2", 1},
      {"1.5 < 2", 1},
      {"2 <= 2.0", 1},
      // String comparisons (including unsigned byte > 0x7F and prefix ordering)
      {"\"\" == \"\"", 1},
      {"\"abc\" == \"abc\"", 1},
      {"\"abc\" == \"abd\"", 0},
      {"\"abc\" != \"abd\"", 1},
      {"\"ab\" < \"abc\"", 1},
      {"\"abc\" > \"ab\"", 1},
      {"\"\\200\" > \"a\"", 1},
      {"\"a\" > \"\\200\"", 0},
      {"\"abc\" >= \"abc\"", 1},
      {"\"ab\" <= \"abc\"", 1},
      // Float predicates
      {"isnan(nan)", 1},
      {"isnan(1.0)", 0},
      {"isinf(inf)", 1},
      {"isinf(-inf)", 1},
      {"isinf(1.0)", 0},
      {"isinf(nan)", 0},
      {"isplusinf(inf)", 1},
      {"isplusinf(-inf)", 0},
      {"isplusinf(1.0)", 0},
      {"ismininf(-inf)", 1},
      {"ismininf(inf)", 0},
      {"ismininf(1.0)", 0},
      // Conditional and with-index
      {"if(true, true, false)", 1},
      {"if(false, true, false)", 0},
      {"with(i = 3, with(i = 4, i == 4) && i == 3)", 1},
  };
  for (const BoolCase& tc : kCases) {
    ExpectBoolExpr(tc.expr, tc.expected);
  }

  // Error propagation across boolean operators.
  constexpr const char* kBoolErrors[] = {
      "(1 / 0) == 1",
      "1 == (1 / 0)",
      "(1.0 / 0.0) == 1.0",
      "1.0 == (1.0 / 0.0)",
      "substr(-1, 1, \"a\") == \"a\"",
      "\"a\" == substr(-1, 1, \"a\")",
      "(1 / 0) != 1",
      "1 != (1 / 0)",
      "(1.0 / 0.0) != 1.0",
      "1.0 != (1.0 / 0.0)",
      "substr(-1, 1, \"a\") != \"a\"",
      "\"a\" != substr(-1, 1, \"a\")",
      "(1 / 0) > 1",
      "1 > (1 / 0)",
      "(1.0 / 0.0) > 1.0",
      "1.0 > (1.0 / 0.0)",
      "substr(-1, 1, \"a\") > \"a\"",
      "\"a\" > substr(-1, 1, \"a\")",
      "(1 / 0) >= 1",
      "1 >= (1 / 0)",
      "(1.0 / 0.0) >= 1.0",
      "1.0 >= (1.0 / 0.0)",
      "substr(-1, 1, \"a\") >= \"a\"",
      "\"a\" >= substr(-1, 1, \"a\")",
      "(1 / 0) < 1",
      "1 < (1 / 0)",
      "(1.0 / 0.0) < 1.0",
      "1.0 < (1.0 / 0.0)",
      "substr(-1, 1, \"a\") < \"a\"",
      "\"a\" < substr(-1, 1, \"a\")",
      "(1 / 0) <= 1",
      "1 <= (1 / 0)",
      "(1.0 / 0.0) <= 1.0",
      "1.0 <= (1.0 / 0.0)",
      "substr(-1, 1, \"a\") <= \"a\"",
      "\"a\" <= substr(-1, 1, \"a\")",
      "!(1 / 0 == 1)",
      "(1 / 0 == 1) && true",
      "true && (1 / 0 == 1)",
      "(1 / 0 == 1) || false",
      "false || (1 / 0 == 1)",
      "isnan(1.0 / 0.0)",
      "isinf(1.0 / 0.0)",
      "isplusinf(1.0 / 0.0)",
      "ismininf(1.0 / 0.0)",
      "if(1 / 0 == 1, true, false)",
      "if(true, 1 / 0 == 1, false)",
      "if(false, true, 1 / 0 == 1)",
      "with(i = 1 / 0, true)",
      "with(i = 1, 1 / 0 == 1)",
  };
  for (const char* expr_str : kBoolErrors) {
    ExpectBoolError(expr_str, CODA_ERROR_EXPRESSION);
  }
}

TEST(CodaExprTest, ConstantIntegerArithmeticBitwiseAndConversions) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct IntCase {
    const char* expr;
    int64_t expected;
  };
  constexpr IntCase kCases[] = {
      {"42", 42},
      {"-42", -42},
      {"+42", 42},
      {"int(42)", 42},
      {"-(2 + 3)", -5},
      {"abs(-7)", 7},
      {"abs(7)", 7},
      {"int(true)", 1},
      {"int(false)", 0},
      {"int(\"12345\")", 12345},
      {"int(\"-99\")", -99},
      {"10 + 3", 13},
      {"10 - 3", 7},
      {"10 * 3", 30},
      {"10 / 3", 3},
      {"10 % 3", 1},
      {"6 & 3", 2},
      {"6 | 3", 7},
      {"max(4, 9)", 9},
      {"max(9, 4)", 9},
      {"min(4, 9)", 4},
      {"min(9, 4)", 4},
      {"if(true, 11, 22)", 11},
      {"if(false, 11, 22)", 22},
      {"with(i = 5, with(j = 7, with(k = 2, i + j * k)))", 19},
  };
  for (const IntCase& tc : kCases) {
    ExpectIntExpr(tc.expr, tc.expected);
  }

  ExpectIntError("int(\"\")", CODA_ERROR_INVALID_FORMAT);
  ExpectIntError("int(\"abc\")", CODA_ERROR_INVALID_FORMAT);

  constexpr const char* kIntErrors[] = {
      "int(1 / 0 == 1)",
      "int(substr(-1, 1, \"a\"))",
      "-(1 / 0)",
      "abs(1 / 0)",
      "(1 / 0) + 1",
      "1 + (1 / 0)",
      "(1 / 0) - 1",
      "1 - (1 / 0)",
      "(1 / 0) * 1",
      "1 * (1 / 0)",
      "(1 / 0) / 1",
      "1 / (1 / 0)",
      "1 / 0",
      "(1 / 0) % 1",
      "1 % (1 / 0)",
      "1 % 0",
      "(1 / 0) & 1",
      "1 & (1 / 0)",
      "(1 / 0) | 1",
      "1 | (1 / 0)",
      "max(1 / 0, 1)",
      "max(1, 1 / 0)",
      "min(1 / 0, 1)",
      "min(1, 1 / 0)",
      "if(1 / 0 == 1, 1, 2)",
      "if(true, 1 / 0, 2)",
      "if(false, 1, 1 / 0)",
      "with(i = 1 / 0, 1)",
      "with(i = 1, 1 / 0)",
  };
  for (const char* expr_str : kIntErrors) {
    ExpectIntError(expr_str, CODA_ERROR_EXPRESSION);
  }
}

TEST(CodaExprTest, ConstantFloatArithmeticMathAndTimeFunctions) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct FloatCase {
    const char* expr;
    double expected;
  };
  constexpr FloatCase kCases[] = {
      {"1.5", 1.5},
      {"-1.5", -1.5},
      {"+1.5", 1.5},
      {"float(1.5)", 1.5},
      {"float(3)", 3.0},
      {"float(\"3.25\")", 3.25},
      {"-(1.5 + 0.5)", -2.0},
      {"abs(-2.5)", 2.5},
      {"abs(2.5)", 2.5},
      {"ceil(2.25)", 3.0},
      {"ceil(-2.25)", -2.0},
      {"floor(2.75)", 2.0},
      {"floor(-2.75)", -3.0},
      {"round(2.4)", 2.0},
      {"round(2.5)", 3.0},
      {"round(-2.4)", -2.0},
      {"round(-2.5)", -3.0},
      {"1.5 + 2", 3.5},
      {"2 + 1.5", 3.5},
      {"5.5 - 2.0", 3.5},
      {"2.5 * 3.0", 7.5},
      {"7.5 / 2.5", 3.0},
      {"5.5 % 2.0", 1.5},
      {"2.0 ^ 3", 8.0},
      {"2.0 ^ -2", 0.25},
      {"2 ^ 0", 1.0},
      {"1.0 ^ 100", 1.0},
      {"1.0 ^ -100", 1.0},
      {"4.0 ^ 0.5", 2.0},
      {"max(1.5, 2.5)", 2.5},
      {"max(2.5, 1.5)", 2.5},
      {"min(1.5, 2.5)", 1.5},
      {"min(2.5, 1.5)", 1.5},
      {"time(\"2000-01-01T00:00:00.000000\", \"yyyy-MM-dd'T'HH:mm:ss.SSSSSS\")",
       0.0},
      {"time(\"2000-01-02T00:00:00\", \"yyyy-MM-dd'T'HH:mm:ss\")", 86400.0},
      {"if(true, 1.5, 2.5)", 1.5},
      {"if(false, 1.5, 2.5)", 2.5},
      {"with(i = 2, float(i) + 0.5)", 2.5},
  };
  for (const FloatCase& tc : kCases) {
    ExpectFloatExpr(tc.expr, tc.expected);
  }

  ExpectFloatError("float(\"\")", CODA_ERROR_INVALID_FORMAT);
  ExpectFloatError("float(\"bad\")", CODA_ERROR_INVALID_FORMAT);
  ExpectFloatError("time(\"\", \"yyyy\")", CODA_ERROR_EXPRESSION);
  ExpectFloatError("time(\"not-a-time\", \"yyyy-MM-dd\")",
                   CODA_ERROR_INVALID_DATETIME);
  {
    // Upstream coda-expr.c returns on len_format == 0 without freeing timestr.
    absl::LeakCheckDisabler leak_disabler;
    ExpectFloatError("time(\"2000-01-01\", \"\")", CODA_ERROR_EXPRESSION);
  }

  constexpr const char* kFloatErrors[] = {
      "1.5 + (1 / 0)",
      "float(1 / 0)",
      "float(substr(-1, 1, \"a\"))",
      "-(1.0 / 0.0)",
      "abs(1.0 / 0.0)",
      "ceil(1.0 / 0.0)",
      "floor(1.0 / 0.0)",
      "round(1.0 / 0.0)",
      "(1.0 / 0.0) + 1.0",
      "1.0 + (1.0 / 0.0)",
      "(1.0 / 0.0) - 1.0",
      "1.0 - (1.0 / 0.0)",
      "(1.0 / 0.0) * 1.0",
      "1.0 * (1.0 / 0.0)",
      "(1.0 / 0.0) / 1.0",
      "1.0 / (1.0 / 0.0)",
      "1.0 / 0.0",
      "(1.0 / 0.0) % 1.0",
      "1.0 % (1.0 / 0.0)",
      "1.0 % 0.0",
      "(1.0 / 0.0) ^ 2",
      "2.0 ^ (1 / 0)",
      "2.0 ^ (1.0 / 0.0)",
      "max(1.0 / 0.0, 1.0)",
      "max(1.0, 1.0 / 0.0)",
      "min(1.0 / 0.0, 1.0)",
      "min(1.0, 1.0 / 0.0)",
      "time(substr(-1, 1, \"a\"), \"yyyy\")",
      "time(\"2000\", substr(-1, 1, \"a\"))",
      "if(1 / 0 == 1, 1.0, 2.0)",
      "if(true, 1.0 / 0.0, 2.0)",
      "if(false, 1.0, 1.0 / 0.0)",
      "with(i = 1 / 0, 1.0)",
      "with(i = 1, 1.0 / 0.0)",
  };
  for (const char* expr_str : kFloatErrors) {
    ExpectFloatError(expr_str, CODA_ERROR_EXPRESSION);
  }
}

TEST(CodaExprTest, ConstantStringOperationsRegexAndStrtime) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // All escape sequences including embedded NUL via \000.
  {
    constexpr std::string_view kExpectedEscaped("\033\a\b\f\n\r\t\v\\\"'A\0z",
                                                14);
    ExpectStringExpr(R"("\e\a\b\f\n\r\t\v\\\"\'\101\000z")", kExpectedEscaped);
    ExpectStringExpr(R"(r"a\nb\tc")", R"(a\nb\tc)");
  }

  struct StringCase {
    const char* expr;
    std::string_view expected;
  };
  constexpr StringCase kCases[] = {
      {"\"\"", ""},
      {"str(\"abc\")", "abc"},
      {"str(0)", "0"},
      {"str(-42)", "-42"},
      {"str(9223372036854775807)", "9223372036854775807"},
      {"\"ab\" + \"cd\"", "abcd"},
      {"\"\" + \"cd\"", "cd"},
      {"\"ab\" + \"\"", "ab"},
      {"\"\" + \"\"", ""},
      {"min(\"a\", \"b\")", "a"},
      {"min(\"b\", \"a\")", "a"},
      {"min(\"\", \"a\")", ""},
      {"min(\"a\", \"\")", ""},
      {"max(\"a\", \"b\")", "b"},
      {"max(\"b\", \"a\")", "b"},
      {"max(\"a\", \"\")", "a"},
      {"max(\"\", \"a\")", "a"},
      {"substr(1, 3, \"abcdef\")", "bcd"},
      {"substr(2, 100, \"abcd\")", "cd"},
      {"substr(0, 0, \"abc\")", ""},
      {"substr(0, 3, \"\")", ""},
      {"substr(10, 3, \"abc\")", ""},
      {"ltrim(\" \\t\\n\\rabc \\t\")", "abc \t"},
      {"rtrim(\" \\tabc \\t\\n\\r\")", " \tabc"},
      {"trim(\" \\t\\n\\rabc \\t\\n\\r\")", "abc"},
      {"trim(\"   \")", ""},
      {"regex(\"([a-z]+)([0-9]+)\", \"ab12\", 0)", "ab12"},
      {"regex(\"([a-z]+)([0-9]+)\", \"ab12\", 1)", "ab"},
      {"regex(\"([a-z]+)([0-9]+)\", \"ab12\", 2)", "12"},
      {"regex(\"([a-z]+)\", \"123\", 1)", ""},
      {"regex(\"([a-z]+)\", \"ab\", 5)", ""},
      {"regex(\"\", \"abc\", 0)", ""},
      {"regex(\"(.*)\", \"\", 0)", ""},
      {"strtime(0)", "2000-01-01T00:00:00.000000"},
      {"strtime(86400.5, \"yyyy-MM-dd HH:mm:ss.SSS\")",
       "2000-01-02 00:00:00.500"},
      {"if(true, \"yes\", \"no\")", "yes"},
      {"if(false, \"yes\", \"no\")", "no"},
      {"with(i = 7, str(i))", "7"},
  };
  for (const StringCase& tc : kCases) {
    ExpectStringExpr(tc.expr, tc.expected);
  }

  ExpectIntExpr("length(\"hello\")", 5);
  ExpectIntExpr("length(\"\")", 0);
  ExpectIntError("length(substr(-1, 1, \"a\"))", CODA_ERROR_EXPRESSION);

  // 2-arg regex (boolean)
  ExpectBoolExpr("regex(\"^a[0-9]+z$\", \"a123z\")", 1);
  ExpectBoolExpr("regex(\"^a[0-9]+z$\", \"nope\")", 0);
  ExpectBoolExpr("regex(\"\", \"abc\")", 1);
  ExpectBoolExpr("regex(\".*\", \"\")", 1);
  ExpectBoolExpr("regex(\"a\", \"\")", 0);
  ExpectBoolError("regex(\"[\", \"abc\")", CODA_ERROR_EXPRESSION);
  ExpectBoolError("regex(\"[\", \"\")", CODA_ERROR_EXPRESSION);
  ExpectBoolError("regex(substr(-1, 1, \"a\"), \"abc\")",
                  CODA_ERROR_EXPRESSION);
  ExpectBoolError("regex(\"a\", substr(-1, 1, \"a\"))", CODA_ERROR_EXPRESSION);

  constexpr const char* kStringErrors[] = {
      "str(1 / 0)",
      "substr(-1, 1, \"a\") + \"b\"",
      "\"a\" + substr(-1, 1, \"b\")",
      "min(substr(-1, 1, \"a\"), \"b\")",
      "min(\"a\", substr(-1, 1, \"b\"))",
      "max(substr(-1, 1, \"a\"), \"b\")",
      "max(\"a\", substr(-1, 1, \"b\"))",
      "substr(1 / 0, 1, \"a\")",
      "substr(-1, 1, \"a\")",
      "substr(0, 1 / 0, \"a\")",
      "substr(0, -1, \"a\")",
      "substr(0, 1, substr(-1, 1, \"a\"))",
      "ltrim(substr(-1, 1, \"a\"))",
      "rtrim(substr(-1, 1, \"a\"))",
      "trim(substr(-1, 1, \"a\"))",
      "regex(substr(-1, 1, \"a\"), \"a\", 0)",
      "regex(\"a\", substr(-1, 1, \"a\"), 0)",
      "regex(\"[\", \"abc\", 0)",
      "regex(\"[\", \"\", 0)",
      "regex(\"a\", \"\", 1 / 0)",
      "regex(\"a\", \"\", substr(-1, 1, \"a\"))",
      "strtime(1.0 / 0.0)",
      "strtime(0.0, substr(-1, 1, \"a\"))",
      "strtime(0.0, \"\")",
      "if(1 / 0 == 1, \"a\", \"b\")",
      "if(true, substr(-1, 1, \"a\"), \"b\")",
      "if(false, \"a\", substr(-1, 1, \"b\"))",
      "with(i = 1 / 0, \"a\")",
      "with(i = 1, substr(-1, 1, \"a\"))",
  };
  for (const char* expr_str : kStringErrors) {
    ExpectStringError(expr_str, CODA_ERROR_EXPRESSION);
  }
}

TEST(CodaExprTest, ProductNavigationArraysBytesSpecialTypesAndVariables) {
  TempFile codadef("expr_test.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildTestCodadefBytes()));
  TempFile bin_file("expr_bin.dat");
  ASSERT_TRUE(bin_file.WriteBytes(BuildBinaryProductBytes()));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(bin_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1. Product metadata expressions.
  ExpectStringExpr("filename()", "expr_bin.dat", &root);
  ExpectStringExpr("productclass()", "ExprTestClass", &root);
  ExpectStringExpr("producttype()", "ExprBin", &root);
  ExpectStringExpr("productformat()", "binary", &root);
  ExpectIntExpr("productversion()", 3, &root);
  ExpectIntExpr("filesize()", 127, &root);

  // 2. Scalar field reads, attributes, special types, and byte/bit queries.
  ExpectIntExpr("int(/i32)", -123456789, &root);
  ExpectIntExpr("int(/u64)", 0x0102030405060708LL, &root);
  ExpectIntExpr("int(/true)", 7, &root);
  ExpectIntExpr("int(/{1})", -123456789, &root);
  ExpectIntExpr("int(./{1})", -123456789, &root);
  ExpectFloatExpr("float(/f64)", 3.5, &root);
  ExpectStringExpr("str(/txt)", "abcdef", &root);
  ExpectStringExpr("str(/txt, 3)", "abc", &root);
  ExpectStringExpr("str(/txt, 100)", "abcdef", &root);
  ExpectStringExpr("str(/empty_txt)", "", &root);
  ExpectIntExpr("int(/tstamp/days)", 1, &root);
  ExpectIntExpr("length(/txt)", 6, &root);
  ExpectIntExpr("length(/empty_txt)", 0, &root);
  ExpectIntExpr("bitsize(/i32)", 32, &root);
  ExpectIntExpr("bytesize(/i32)", 4, &root);
  ExpectIntExpr("bitsize(/raw5)", 5, &root);
  ExpectIntExpr("bitoffset(/i32)", 64, &root);
  ExpectIntExpr("byteoffset(/i32)", 8, &root);
  ExpectStringExpr("bytes(/txt)", "abcdef", &root);
  ExpectStringExpr("bytes(/txt, 4)", "abcd", &root);
  ExpectStringExpr("bytes(/txt, 2, 3)", "cde", &root);
  ExpectStringExpr("bytes(/empty_txt)", "", &root);
  ExpectStringExpr("bytes(/raw5)", std::string_view("\x15", 1), &root);
  ExpectStringError("bytes(/txt, 0)", CODA_ERROR_EXPRESSION, &root);
  ExpectStringError("bytes(/txt, -1)", CODA_ERROR_EXPRESSION, &root);
  ExpectStringError("bytes(/txt, 1000)", CODA_ERROR_OUT_OF_BOUNDS_READ, &root);

  // 3. Cursor navigation with eval_node, at(), :, .., and [idx].
  // Note: eval_cursor(expr_goto_array_element) with operand[0] == NULL resets
  // info->cursor to root (coda-expr.c:4059), so root-less [idx] indexing only
  // succeeds when the product's root node itself is an array (tested separately
  // via a top-level .codadef array below). Here cur is /arr under a record
  // root, so use /arr[idx] and test goto(".."), goto("/"), at(), and :.
  {
    coda_cursor cur = root;
    ScopedExpr goto_arr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("/arr", goto_arr.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_node(goto_arr.get(), &cur));
    ExpectIntExpr("int(/arr[2])", 30, &cur);
    ExpectIntExpr("at(/arr[2], at(:, int(/i32)))", -123456789, &cur);
    ExpectStringExpr("at(/txt, str(.))", "abcdef", &cur);
    ExpectFloatExpr("at(/f64, float(.))", 3.5, &cur);
    ExpectBoolExpr("at(/i32, int(.) < 0)", 1, &cur);
    ExpectIntExpr("int(/arr[2]/../../i32)", -123456789, &cur);

    ScopedExpr goto_elem;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("/arr[1]", goto_elem.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_node(goto_elem.get(), &cur));
    ExpectIntExpr("int(.)", 20, &cur);
    ExpectIntExpr("index(.)", 1, &cur);

    ScopedExpr goto_parent;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("..", goto_parent.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_node(goto_parent.get(), &cur));
    ExpectIntExpr("numelements(.)", 4, &cur);

    ScopedExpr goto_root;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("/", goto_root.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_node(goto_root.get(), &cur));
    ExpectIntError("index(.)", CODA_ERROR_INVALID_ARGUMENT, &cur);
  }

  // 4. Array dims, quantifiers, reductions, and unboundindex.
  ExpectIntExpr("numdims(/mat2d)", 2, &root);
  ExpectIntExpr("dim(/mat2d, 0)", 2, &root);
  ExpectIntExpr("dim(/mat2d, 1)", 3, &root);
  ExpectIntExpr("numelements(/mat2d)", 6, &root);
  ExpectIntExpr("numelements(/empty_arr)", 0, &root);
  ExpectIntError("dim(/mat2d, -1)", CODA_ERROR_EXPRESSION, &root);
  ExpectIntError("dim(/mat2d, 2)", CODA_ERROR_EXPRESSION, &root);
  ExpectIntError("numdims(/i32)", CODA_ERROR_INVALID_TYPE, &root);

  ExpectBoolExpr("exists(/i32)", 1, &root);
  ExpectBoolExpr("exists(/no_such_field)", 0, &root);
  EXPECT_EQ(CODA_SUCCESS, coda_errno);
  ExpectBoolExpr("exists(/opt_missing)", 0, &root);
  ExpectBoolExpr("exists(/tstamp)", 1, &root);
  ExpectBoolError("exists(/arr[1 / 0])", CODA_ERROR_EXPRESSION, &root);

  ExpectBoolExpr("all(/arr, int(.) > 0)", 1, &root);
  ExpectBoolExpr("all(/arr, int(.) < 30)", 0, &root);
  ExpectBoolExpr("all(/empty_arr, int(.) == 0)", 1, &root);

  ExpectBoolExpr("exists(/arr, int(.) == 30)", 1, &root);
  ExpectBoolExpr("exists(/arr, int(.) == 999)", 0, &root);
  ExpectBoolExpr("exists(/empty_arr, int(.) == 0)", 0, &root);

  ExpectIntExpr("count(/arr, int(.) >= 20)", 3, &root);
  ExpectIntExpr("count(/empty_arr, int(.) == 0)", 0, &root);
  ExpectIntExpr("add(/arr, int(.))", 100, &root);
  ExpectIntExpr("add(/empty_arr, int(.))", 0, &root);
  ExpectIntExpr("max(/arr, int(.))", 40, &root);
  ExpectIntExpr("max(/empty_arr, int(.))", 0, &root);
  ExpectIntExpr("min(/arr, int(.))", 10, &root);
  ExpectIntExpr("min(/empty_arr, int(.))", 0, &root);
  ExpectIntExpr("index(/arr, int(.) == 30)", 2, &root);
  ExpectIntExpr("index(/arr, int(.) == 999)", -1, &root);
  ExpectIntExpr("index(/empty_arr, int(.) == 0)", -1, &root);

  ExpectIntExpr("unboundindex(/arr, int(.) == 30)", 2, &root);
  ExpectIntExpr("unboundindex(/arr, int(.) == 10, int(.) == 99)", -1, &root);
  ExpectIntExpr("unboundindex(/arr, int(.) == 20, int(.) == 99)", -1, &root);
  ExpectIntError("unboundindex(/i32, int(.) == 0)", CODA_ERROR_INVALID_TYPE,
                 &root);

  ExpectFloatExpr("add(/mat2d, float(.))", 12.0, &root);
  ExpectFloatExpr("add(/empty_arr, float(.))", 0.0, &root);
  ExpectFloatExpr("max(/mat2d, float(.))", 9.5, &root);
  ExpectFloatExpr("min(/mat2d, float(.))", -2.5, &root);
  ExpectBoolExpr("isnan(max(/empty_arr, float(.)))", 1, &root);
  ExpectBoolExpr("isnan(min(/empty_arr, float(.)))", 1, &root);

  ExpectStringExpr("add(/str_arr, str(.))", "betaalfagama", &root);
  ExpectStringExpr("add(/empty_arr, \"x\")", "", &root);
  ExpectStringExpr("max(/str_arr, str(.))", "gama", &root);
  // String array min starts from empty string "", so "" compares smaller than
  // every non-empty string element and returns "".
  ExpectStringExpr("min(/str_arr, str(.))", "", &root);

  // 5. Named regex capture using NUL-terminated str(/grp_name) to avoid
  // uninitialized-tail reads on non-NUL constant strings.
  ExpectStringExpr("regex(\"(?<grp>a+)\", \"baaac\", str(/grp_name))", "aaa",
                   &root);
  ExpectStringError("regex(\"(?<other>a+)\", \"\", str(/grp_name))",
                    CODA_ERROR_EXPRESSION, &root);

  // 6. Boundary check disabled branch in eval_cursor(expr_goto_array_element).
  {
    const int orig_bc = coda_get_option_perform_boundary_checks();
    const absl::Cleanup restore_bc = [orig_bc] {
      coda_set_option_perform_boundary_checks(orig_bc);
    };
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
    ExpectIntExpr("int(/arr[2])", 30, &root);
    ExpectIntError("int(/arr[-1])", CODA_ERROR_ARRAY_OUT_OF_BOUNDS, &root);
    ExpectIntError("int(/arr[4])", CODA_ERROR_ARRAY_OUT_OF_BOUNDS, &root);
    ExpectIntError("int(/i32[0])", CODA_ERROR_INVALID_TYPE, &root);
  }

  // 7. Product variables and eval_void statements.
  ExpectIntExpr("$scalar_var", 99, &root);
  ExpectIntExpr("$arr_var[2]", 30, &root);
  ExpectBoolExpr("exists($arr_var, $arr_var == 30)", 1, &root);
  ExpectBoolExpr("exists($arr_var, $arr_var == 999)", 0, &root);
  ExpectIntExpr("index($arr_var, $arr_var == 30)", 2, &root);
  ExpectIntExpr("index($arr_var, $arr_var == 999)", -1, &root);
  ExpectBoolError("exists($arr_var, exists($scalar_var, $scalar_var == 99))",
                  CODA_ERROR_EXPRESSION, &root);
  ExpectIntError("index($arr_var, index($scalar_var, $scalar_var == 99) == 0)",
                 CODA_ERROR_EXPRESSION, &root);
  ExpectBoolError("exists($arr_var, $arr_var[0] == 10)", CODA_ERROR_EXPRESSION,
                  &root);
  ExpectIntError("$missing_var", CODA_ERROR_DATA_DEFINITION, &root);
  ExpectBoolError("exists($missing_var, true)", CODA_ERROR_DATA_DEFINITION,
                  &root);
  ExpectIntError("index($missing_var, true)", CODA_ERROR_DATA_DEFINITION,
                 &root);

  {
    ScopedExpr void_seq;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(
                  "goto(/arr); $scalar_var = with(i = 1, int(.[i])); "
                  "for i = 0 to 3 do $arr_var[i] = i + 1; "
                  "for i = 3 to 1 step -2 do $arr_var[i] = 77",
                  void_seq.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_void(void_seq.get(), &root));
    ExpectIntExpr("$scalar_var", 20, &root);
    ExpectIntExpr("$arr_var[0]", 1, &root);
    ExpectIntExpr("$arr_var[1]", 77, &root);
    ExpectIntExpr("$arr_var[2]", 3, &root);
    ExpectIntExpr("$arr_var[3]", 77, &root);

    ScopedExpr step_zero;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                "for i = 0 to 2 step 0 do $scalar_var = i",
                                step_zero.OutParam()));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_expression_eval_void(step_zero.get(), &root));
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  }

  // 8. Cursor error decoration when evaluation fails after moving cursor, plus
  // asciiline rejection on non-ASCII files.
  ExpectIntError("int(/arr[99])", CODA_ERROR_ARRAY_OUT_OF_BOUNDS, &root);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("at /arr"));
  ExpectStringError("str(asciiline[0])", CODA_ERROR_EXPRESSION, &root);
  ExpectBoolError("at(/missing, true)", CODA_ERROR_INVALID_NAME, &root);
  ExpectFloatError("at(/missing, 1.0)", CODA_ERROR_INVALID_NAME, &root);
  ExpectStringError("at(/missing, \"x\")", CODA_ERROR_INVALID_NAME, &root);
  ExpectBoolError("at(/i32, 1 / 0 == 1)", CODA_ERROR_EXPRESSION, &root);
  ExpectFloatError("at(/i32, 1.0 / 0.0)", CODA_ERROR_EXPRESSION, &root);
  ExpectStringError("at(/i32, substr(-1, 1, \"a\"))", CODA_ERROR_EXPRESSION,
                    &root);
  ExpectIntError("int(/txt)", CODA_ERROR_INVALID_TYPE, &root);
  ExpectFloatError("float(/txt)", CODA_ERROR_INVALID_TYPE, &root);
  ExpectStringError("str(/i32)", CODA_ERROR_INVALID_TYPE, &root);
  ExpectStringError("str(/txt, 1 / 0)", CODA_ERROR_EXPRESSION, &root);
  ExpectIntError("length(/i32)", CODA_ERROR_INVALID_TYPE, &root);
  ExpectStringError("str(@missing_attr)", CODA_ERROR_INVALID_NAME, &root);
  ExpectStringError("str(/i32@missing_attr)", CODA_ERROR_INVALID_NAME, &root);
  {
    coda_cursor cur = root;
    ScopedExpr bad_parent;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("..", bad_parent.OutParam()));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_expression_eval_node(bad_parent.get(), &cur));
    EXPECT_EQ(CODA_ERROR_NO_PARENT, coda_errno);
  }
}

TEST(CodaExprTest, AsciiLineAndRawXmlProduct) {
  TempFile codadef("expr_asc.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildTestCodadefBytes()));
  TempFile asc_file("expr_asc.txt");
  ASSERT_TRUE(asc_file.WriteText("EXPRASC1\nline two\n"));
  TempFile xml_file("expr_plain.xml");
  ASSERT_TRUE(xml_file.WriteText(
      "<?xml version=\"1.0\"?><root rec_attr=\"rootval\"><txt "
      "tag=\"atr\">abcdef</txt></root>"));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct asc_product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(asc_file.c_str(), asc_product.OutParam()));
  coda_cursor asc_root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_set_product(&asc_root, asc_product.get()));
  {
    // Upstream bug: coda_ascii_cursor_set_asciilines() (coda-ascii-cursor.c:
    // 611-625) forgets coda_type_release(asciiline) after
    // coda_type_array_set_base_type(), leaving retain_count == 2 so
    // coda_ascii_close() leaks the 72-byte coda_type_text on first init.
    absl::LeakCheckDisabler leak_disabler;
    ExpectStringExpr("str(asciiline[0])", "EXPRASC1\n", &asc_root);
  }
  ExpectStringExpr("str(asciiline[1])", "line two\n", &asc_root);
  ExpectIntExpr("length(asciiline[1])", 9, &asc_root);
  ExpectStringExpr("productformat()", "ascii", &asc_root);
  ExpectIntExpr("productversion()", 2, &asc_root);

  ScopedProduct xml_product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), xml_product.OutParam()));
  coda_cursor xml_root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_set_product(&xml_root, xml_product.get()));
  ExpectStringExpr("productclass()", "", &xml_root);
  ExpectStringExpr("producttype()", "", &xml_root);
  ExpectStringExpr("productformat()", "xml", &xml_root);
  ExpectIntExpr("productversion()", -1, &xml_root);
  ExpectStringExpr("str(/root@rec_attr)", "rootval", &xml_root);
  ExpectStringExpr("at(/root, str(@rec_attr))", "rootval", &xml_root);
  ExpectStringExpr("str(/root/txt@tag)", "atr", &xml_root);
  ExpectStringError("str(/root@missing_attr)", CODA_ERROR_INVALID_NAME,
                    &xml_root);
  ExpectIntExpr("bitoffset(/root)", -1, &xml_root);
  {
    ScopedExpr byte_off_expr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                "byteoffset(/root)", byte_off_expr.OutParam()));
    int64_t val = 0;
    EXPECT_EQ(
        -1, coda_expression_eval_integer(byte_off_expr.get(), &xml_root, &val));
  }
}

TEST(CodaExprTest, PrintExpressionPlainXmlHtmlAndRoundTrip) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Round-trip representative expressions covering all node types and
  // precedence parenthesization rules (expressions using &&/|| are checked via
  // kPrintOnlyExprs below because coda_expression_print emits "and"/"or", which
  // coda_expression_from_string does not parse back).
  constexpr const char* kRoundTripExprs[] = {
      "(1 + 2) * 3 - (4 / 2) % 3",
      "-(1 + 2) + abs(-3) + -(1 - 2) + -(1 & 2) + -(1 | 2)",
      "((1 | 2) & 3) == 3",
      "(1 | 2) == 3",
      "(1 + 2) ^ 2 + -(2 ^ 3)",
      "1 != 2",
      "1 < 2",
      "!(1 < 2)",
      "1 <= 2",
      "!(1 > 2)",
      "1 >= 2",
      "ceil(1.2) + floor(1.8) + round(2.5) + min(1.5, 2.5) + max(1.5, 2.5)",
      "float(int(\"12\")) + time(\"2000-01-01\", \"yyyy-MM-dd\")",
      "isinf(inf)",
      "isplusinf(inf)",
      "ismininf(-inf)",
      "ltrim(rtrim(trim(substr(0, 2, \"  ab  \")))) + strtime(0) + strtime(0, "
      "\"yyyy\")",
      "regex(\"a\", \"a\")",
      "regex(\"(a)\", \"a\", 1) + regex(\"(?<g>a)\", \"a\", \"g\")",
      "str(/txt) + str(/txt, 3) + bytes(/txt) + bytes(/txt, 2) + bytes(/txt, "
      "1, 2)",
      "length(/txt) + bitsize(/i32) + bytesize(/i32) + bitoffset(/i32) + "
      "byteoffset(/i32)",
      "filesize() + productversion() + index(/arr[0]) + numdims(/mat2d) + "
      "numelements(/mat2d) + dim(/mat2d, 0)",
      "filename() + productclass() + productformat() + producttype()",
      "exists(/i32)",
      "exists(/arr, int(.) > 0)",
      "all(/arr, int(.) > 0)",
      "add(/arr, int(.)) + max(/arr, int(.)) + min(/arr, int(.)) + count(/arr, "
      "int(.) > 0) + index(/arr, int(.) == 1) + unboundindex(/arr, int(.) == "
      "1)",
      "exists($v, $v == 1)",
      "index($v, $v == 1) == 0",
      "if(true, at(/a, int(./b@c)), with(i = 1, int(asciiline[i])))",
      "int(./../a) + int(:)",
      "goto(/a); $x = 1; for i = 0 to 3 step 2 do $y[i] = i",
      "goto([0]); $x = $y[0]; for i = 0 to 1 do goto(@attr)",
  };

  for (const char* expr_str : kRoundTripExprs) {
    ScopedExpr orig;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(expr_str, orig.OutParam()))
        << expr_str;
    const std::string printed = PrintWith(orig.get(), &coda_expression_print);
    ScopedExpr reparsed;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(printed.c_str(), reparsed.OutParam()))
        << "printed=" << printed;
    EXPECT_EQ(1, coda_expression_is_equal(orig.get(), reparsed.get()))
        << "orig=" << expr_str << " printed=" << printed;

    // Also exercise HTML and XML printers on every expression node kind.
    const std::string html = PrintWith(orig.get(), &coda_expression_print_html);
    const std::string xml = PrintWith(orig.get(), &coda_expression_print_xml);
    EXPECT_FALSE(html.empty()) << expr_str;
    EXPECT_FALSE(xml.empty()) << expr_str;
  }

  constexpr std::pair<const char*, std::string_view> kPrintOnlyExprs[] = {
      {"true && false || !false", "true and false or !false"},
      {"!(true && false) && !(false || true)",
       "!(true and false) and !(false or true)"},
  };
  for (const auto& [expr_str, expected_plain] : kPrintOnlyExprs) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(expr_str, expr.OutParam()))
        << expr_str;
    EXPECT_EQ(expected_plain, PrintWith(expr.get(), &coda_expression_print));
    EXPECT_FALSE(PrintWith(expr.get(), &coda_expression_print_html).empty());
    EXPECT_FALSE(PrintWith(expr.get(), &coda_expression_print_xml).empty());
  }

  // TODO(schwehr): coda-expr.c:4520 passes NULL identifier to print("%s", ...)
  // when printing goto_{array_element,attribute,field} nodes with NULL
  // identifier (e.g., "{0}" from coda-expr-parser.y:1006). Exercise the branch
  // without pinning glibc-specific "(null)" formatting.
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                "int(./../a/{0}) + int(:)", expr.OutParam()));
    const std::string plain = PrintWith(expr.get(), &coda_expression_print);
    EXPECT_THAT(plain, HasSubstr("int(./../a/"));
    EXPECT_THAT(plain, HasSubstr(") + int(:)"));
    EXPECT_FALSE(PrintWith(expr.get(), &coda_expression_print_html).empty());
    EXPECT_FALSE(PrintWith(expr.get(), &coda_expression_print_xml).empty());
  }

  // Test isnan(nan) separately because coda_expression_is_equal compares float
  // constants with == (where NaN == NaN is false).
  {
    ScopedExpr nan_expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("isnan(nan)", nan_expr.OutParam()));
    EXPECT_EQ("isnan(nan)", PrintWith(nan_expr.get(), &coda_expression_print));
    EXPECT_EQ("<b>isnan</b>(nan)",
              PrintWith(nan_expr.get(), &coda_expression_print_html));
    EXPECT_EQ("isnan(nan)",
              PrintWith(nan_expr.get(), &coda_expression_print_xml));
  }

  // Verify escaping in plain, XML, and HTML modes.
  {
    ScopedExpr esc;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(R"("\e\a\b\f\n\r\t\v\\\"\001 <>&")",
                                          esc.OutParam()));
    EXPECT_EQ(R"("\e\a\b\f\n\r\t\v\\\"\001 <>&")",
              PrintWith(esc.get(), &coda_expression_print));
    EXPECT_EQ(R"(&quot;\e\a\b\f\n\r\t\v\\\&quot;\001&nbsp;&lt;&gt;&amp;&quot;)",
              PrintWith(esc.get(), &coda_expression_print_xml));
    EXPECT_EQ(R"(&quot;\e\a\b\f\n\r\t\v\\\&quot;\001&nbsp;&lt;&gt;&amp;&quot;)",
              PrintWith(esc.get(), &coda_expression_print_html));
  }
  {
    ScopedExpr empty;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("\"\"", empty.OutParam()));
    EXPECT_EQ("\"\"", PrintWith(empty.get(), &coda_expression_print));
  }
}

// TODO(schwehr): 90.4% line coverage in libcoda/coda-expr.c (2806/3104);
// remaining missed lines are malloc/strdup OOM branches, defensive AST error
// guards, and unreachable default switch arms.

}  // namespace
}  // namespace stcorp_coda
