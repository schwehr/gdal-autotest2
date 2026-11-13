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
// Tests CODA expression parsing (libcoda/coda-expr-parser.c, generated from
// libcoda/coda-expr-parser.y and libcoda/coda-expr-tokenizer.l).
//
// google3 build caveats:
//   - Built with -DCODA_MAX_RECURSION_DEPTH=100, which caps AST nesting depth
//     in coda_expression_new() and triggers YYERROR in yyparse().
//   - Bison 3.5.1 uses YYINITDEPTH=200 and YYMAXDEPTH=10000 for the parser
//     state/value stacks.
//   - Generated #line directives pointing to "libcoda/coda-expr-parser.y"
//     cause Blaze LCOV to emit 0 DA lines for coda-expr-parser.c; standalone
//     llvm-cov measurement with #line directives stripped verifies 1773/1879
//     (94.4%, 7/7 functions).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-parser.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-parser.y
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-tokenizer.l
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/libcoda/coda.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

thread_local std::string* g_print_buffer = nullptr;

int CapturePrintedExpression(const char* fmt, ...) {
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
  const std::size_t old_size = g_print_buffer->size();
  g_print_buffer->resize(old_size + static_cast<std::size_t>(needed));
  std::vsnprintf(g_print_buffer->data() + old_size,
                 static_cast<std::size_t>(needed) + 1, fmt, ap);
  va_end(ap);
  return needed;
}

std::string PrintExpressionToString(const coda_expression* expr) {
  std::string out;
  g_print_buffer = &out;
  const absl::Cleanup reset_buffer = [] { g_print_buffer = nullptr; };
  EXPECT_EQ(CODA_SUCCESS,
            coda_expression_print(expr, &CapturePrintedExpression));
  return out;
}

void ParseAndCheckMetadata(std::string_view expr_text,
                           coda_expression_type expected_type,
                           int expected_is_constant, ScopedExpr* out_expr) {
  const std::string text_copy(expr_text);
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(text_copy.c_str(),
                                                      out_expr->OutParam()))
      << "expr: " << expr_text;
  ASSERT_NE(nullptr, out_expr->get()) << "expr: " << expr_text;

  coda_expression_type actual_type = coda_expression_void;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_get_type(out_expr->get(), &actual_type))
      << "expr: " << expr_text;
  EXPECT_EQ(expected_type, actual_type) << "expr: " << expr_text;
  EXPECT_EQ(expected_is_constant, coda_expression_is_constant(out_expr->get()))
      << "expr: " << expr_text;
}

TEST(CodaExprParserTest, FromStringRejectsNullArguments) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedExpr expr;
  EXPECT_EQ(-1, coda_expression_from_string(nullptr, expr.OutParam()));
  EXPECT_EQ(nullptr, expr.get());
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("invalid expression string argument"));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_from_string("1", nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("invalid expression argument"));
}

TEST(CodaExprParserTest, ParsesAndEvaluatesBooleanExpressions) {
  struct BoolCase {
    std::string_view expr;
    int expected;
  };
  constexpr BoolCase kCases[] = {
      // Literals and logical operators.
      {.expr = "true", .expected = 1},
      {.expr = "false", .expected = 0},
      {.expr = "(true)", .expected = 1},
      {.expr = "!true", .expected = 0},
      {.expr = "!false", .expected = 1},
      {.expr = "!!true", .expected = 1},
      {.expr = "true && true", .expected = 1},
      {.expr = "true && false", .expected = 0},
      {.expr = "false || true", .expected = 1},
      {.expr = "false || false", .expected = 0},
      {.expr = "!false && true", .expected = 1},
      // In CODA grammar, && and || share equal left-associative precedence.
      {.expr = "true || false && false", .expected = 0},
      {.expr = "true || (false && false)", .expected = 1},

      // Integer comparisons.
      {.expr = "2 >= 1", .expected = 1},
      {.expr = "1 >= 2", .expected = 0},
      {.expr = "1 <= 2", .expected = 1},
      {.expr = "2 <= 1", .expected = 0},
      {.expr = "2 == 2", .expected = 1},
      {.expr = "1 == 2", .expected = 0},
      {.expr = "1 != 2", .expected = 1},
      {.expr = "2 != 2", .expected = 0},
      {.expr = "1 < 2", .expected = 1},
      {.expr = "2 < 1", .expected = 0},
      {.expr = "2 > 1", .expected = 1},
      {.expr = "1 > 2", .expected = 0},

      // Float/float, float/int, and int/float comparisons.
      {.expr = "2.5 >= 1.5", .expected = 1},
      {.expr = "2.5 >= 2", .expected = 1},
      {.expr = "3 >= 2.5", .expected = 1},
      {.expr = "1.5 <= 2.5", .expected = 1},
      {.expr = "1.5 <= 2", .expected = 1},
      {.expr = "2 <= 2.5", .expected = 1},
      {.expr = "1.5 == 1.5", .expected = 1},
      {.expr = "2.0 == 2", .expected = 1},
      {.expr = "2 == 2.0", .expected = 1},
      {.expr = "1.5 != 2.5", .expected = 1},
      {.expr = "1.5 != 2", .expected = 1},
      {.expr = "2 != 2.5", .expected = 1},
      {.expr = "1.5 < 2.5", .expected = 1},
      {.expr = "1.5 < 2", .expected = 1},
      {.expr = "1 < 1.5", .expected = 1},
      {.expr = "2.5 > 1.5", .expected = 1},
      {.expr = "2.5 > 2", .expected = 1},
      {.expr = "3 > 2.5", .expected = 1},

      // String comparisons.
      {.expr = "\"abc\" == \"abc\"", .expected = 1},
      {.expr = "\"abc\" != \"def\"", .expected = 1},
      {.expr = "\"abc\" < \"abd\"", .expected = 1},
      {.expr = "\"abd\" > \"abc\"", .expected = 1},
      {.expr = "\"abc\" >= \"abc\"", .expected = 1},
      {.expr = "\"abc\" <= \"abd\"", .expected = 1},

      // Float classification predicates.
      {.expr = "isnan(nan)", .expected = 1},
      {.expr = "isnan(1.0)", .expected = 0},
      {.expr = "isinf(inf)", .expected = 1},
      {.expr = "isinf(-inf)", .expected = 1},
      {.expr = "isinf(1.0)", .expected = 0},
      {.expr = "isplusinf(inf)", .expected = 1},
      {.expr = "isplusinf(-inf)", .expected = 0},
      {.expr = "ismininf(-inf)", .expected = 1},
      {.expr = "ismininf(inf)", .expected = 0},

      // Boolean regex, conditional, and index-binding with().
      {.expr = "regex(\"^a[0-9]+$\", \"a42\")", .expected = 1},
      {.expr = "regex(\"^a[0-9]+$\", \"b42\")", .expected = 0},
      {.expr = "if(true, false, true)", .expected = 0},
      {.expr = "if(false, false, true)", .expected = 1},
      {.expr = "with(i = 3, i == 3)", .expected = 1},
      {.expr = "with(j = 4, j < 4)", .expected = 0},
  };

  for (const BoolCase& tc : kCases) {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        tc.expr, coda_expression_boolean, /*expected_is_constant=*/1, &expr));
    int value = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_bool(expr.get(), nullptr, &value))
        << "expr: " << tc.expr;
    EXPECT_EQ(tc.expected, value) << "expr: " << tc.expr;
  }
}

TEST(CodaExprParserTest, ParsesAndEvaluatesIntegerExpressions) {
  struct IntCase {
    std::string_view expr;
    int64_t expected;
  };
  constexpr IntCase kCases[] = {
      {.expr = "0", .expected = 0},
      {.expr = "42", .expected = 42},
      {.expr = "-5", .expected = -5},
      {.expr = "+5", .expected = 5},
      {.expr = "(7)", .expected = 7},
      {.expr = "-(1 + 2)", .expected = -3},
      {.expr = "+(1 + 2)", .expected = 3},
      {.expr = "int(42)", .expected = 42},
      {.expr = "int(true)", .expected = 1},
      {.expr = "int(false)", .expected = 0},
      {.expr = "int(\"123\")", .expected = 123},
      {.expr = "int(\"-45\")", .expected = -45},
      {.expr = "1 + 2", .expected = 3},
      {.expr = "5 - 2", .expected = 3},
      {.expr = "3 * 4", .expected = 12},
      {.expr = "9 / 2", .expected = 4},
      {.expr = "9 % 4", .expected = 1},
      {.expr = "6 & 3", .expected = 2},
      {.expr = "4 | 1", .expected = 5},
      // Operator precedence & associativity checks.
      {.expr = "2 + 3 * 4", .expected = 14},
      {.expr = "(2 + 3) * 4", .expected = 20},
      {.expr = "10 - 3 - 2", .expected = 5},
      {.expr = "1 + 2 & 3", .expected = 3},
      // In CODA grammar, & and | share equal left-associative precedence.
      {.expr = "4 | 2 & 3", .expected = 2},
      {.expr = "4 | (2 & 3)", .expected = 6},
      // Integer functions.
      {.expr = "abs(-7)", .expected = 7},
      {.expr = "abs(7)", .expected = 7},
      {.expr = "max(3, 7)", .expected = 7},
      {.expr = "min(3, 7)", .expected = 3},
      {.expr = "length(\"hello\")", .expected = 5},
      {.expr = "length(\"\")", .expected = 0},
      {.expr = "if(2 > 1, 10, 20)", .expected = 10},
      {.expr = "if(2 < 1, 10, 20)", .expected = 20},
      {.expr = "with(k = 5, k * 2)", .expected = 10},
      {.expr = "with(i = 2, with(j = 3, i + j))", .expected = 5},
  };

  for (const IntCase& tc : kCases) {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        tc.expr, coda_expression_integer, /*expected_is_constant=*/1, &expr));
    int64_t value = -999;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), nullptr, &value))
        << "expr: " << tc.expr;
    EXPECT_EQ(tc.expected, value) << "expr: " << tc.expr;
  }
}

TEST(CodaExprParserTest, ParsesAndEvaluatesFloatExpressions) {
  struct FloatCase {
    std::string_view expr;
    double expected;
  };
  constexpr FloatCase kCases[] = {
      // Tokenizer FLOAT literal forms.
      {.expr = "1.25", .expected = 1.25},
      {.expr = ".5", .expected = 0.5},
      {.expr = "2.", .expected = 2.0},
      {.expr = "1e3", .expected = 1000.0},
      {.expr = "2.5e-1", .expected = 0.25},
      {.expr = "2.E+1", .expected = 20.0},
      {.expr = ".25e2", .expected = 25.0},
      // Unary, parens, and conversions.
      {.expr = "-1.5", .expected = -1.5},
      {.expr = "+1.5", .expected = 1.5},
      {.expr = "(1.5)", .expected = 1.5},
      {.expr = "-(1.5 + 0.5)", .expected = -2.0},
      {.expr = "+(1.5 + 0.5)", .expected = 2.0},
      {.expr = "float(1.5)", .expected = 1.5},
      {.expr = "float(3)", .expected = 3.0},
      {.expr = "float(\"2.5\")", .expected = 2.5},
      // Binary arithmetic across float/float, float/int, and int/float.
      {.expr = "1.5 + 2.5", .expected = 4.0},
      {.expr = "1.5 + 2", .expected = 3.5},
      {.expr = "2 + 1.5", .expected = 3.5},
      {.expr = "5.5 - 2.0", .expected = 3.5},
      {.expr = "5.5 - 2", .expected = 3.5},
      {.expr = "5 - 2.5", .expected = 2.5},
      {.expr = "2.5 * 3.0", .expected = 7.5},
      {.expr = "2.5 * 4", .expected = 10.0},
      {.expr = "4 * 2.5", .expected = 10.0},
      {.expr = "7.5 / 2.5", .expected = 3.0},
      {.expr = "7.5 / 3", .expected = 2.5},
      {.expr = "7 / 2.0", .expected = 3.5},
      {.expr = "7.5 % 2.0", .expected = 1.5},
      {.expr = "7.5 % 2", .expected = 1.5},
      {.expr = "7 % 2.5", .expected = 2.0},
      // Power operator combinations and precedence (UNARY > ^ > *).
      {.expr = "2.0 ^ 3.0", .expected = 8.0},
      {.expr = "2.0 ^ 3", .expected = 8.0},
      {.expr = "2 ^ 3.0", .expected = 8.0},
      {.expr = "2 ^ 3", .expected = 8.0},
      {.expr = "-2 ^ 2", .expected = 4.0},
      {.expr = "-(2 ^ 2)", .expected = -4.0},
      {.expr = "2.0 * 3.0 ^ 2", .expected = 18.0},
      // Float functions.
      {.expr = "abs(-2.5)", .expected = 2.5},
      {.expr = "ceil(1.2)", .expected = 2.0},
      {.expr = "floor(1.8)", .expected = 1.0},
      {.expr = "round(1.6)", .expected = 2.0},
      {.expr = "round(-1.6)", .expected = -2.0},
      {.expr = "max(1.5, 2.5)", .expected = 2.5},
      {.expr = "max(2.5, 2)", .expected = 2.5},
      {.expr = "max(2, 2.5)", .expected = 2.5},
      {.expr = "min(1.5, 2.5)", .expected = 1.5},
      {.expr = "min(2.5, 2)", .expected = 2.0},
      {.expr = "min(2, 2.5)", .expected = 2.0},
      {.expr = "time(\"2000-01-01 00:01:30\", \"yyyy-MM-dd HH:mm:ss\")",
       .expected = 90.0},
      {.expr = "if(true, 1.5, 2.5)", .expected = 1.5},
      {.expr = "if(true, 1.5, 2)", .expected = 1.5},
      // Use true condition so eval_integer evaluates int op2 without hitting
      // eval_integer's assert(0) on float op3 (upstreams bug on mixed-type if).
      {.expr = "if(true, 1, 2.5) + 0.0", .expected = 1.0},
      {.expr = "with(i = 2, i + 0.5)", .expected = 2.5},
  };

  for (const FloatCase& tc : kCases) {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        tc.expr, coda_expression_float, /*expected_is_constant=*/1, &expr));
    double value = -999.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &value))
        << "expr: " << tc.expr;
    EXPECT_DOUBLE_EQ(tc.expected, value) << "expr: " << tc.expr;
  }

  // Standalone if(false, 1, 2.5) exercises Bison rule (boolexpr, intexpr,
  // floatexpr) while pinning coda-expr.c:529 (expr_if inherits op2's type).
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(
        ParseAndCheckMetadata("if(false, 1, 2.5)", coda_expression_integer,
                              /*expected_is_constant=*/1, &expr));
  }

  // Special float constants: nan, inf, -inf.
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "nan", coda_expression_float, /*expected_is_constant=*/1, &expr));
    double value = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &value));
    EXPECT_EQ(1, coda_isNaN(value));
  }
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "inf", coda_expression_float, /*expected_is_constant=*/1, &expr));
    double value = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &value));
    EXPECT_EQ(1, coda_isPlusInf(value));
  }
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "-inf", coda_expression_float, /*expected_is_constant=*/1, &expr));
    double value = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &value));
    EXPECT_EQ(1, coda_isMinInf(value));
  }
}

TEST(CodaExprParserTest, ParsesAndEvaluatesStringExpressions) {
  struct StringCase {
    std::string_view expr;
    std::string_view expected;
  };
  constexpr StringCase kCases[] = {
      {.expr = "\"\"", .expected = ""},
      {.expr = "\"hello\"", .expected = "hello"},
      {.expr = "\"\\a\\b\\e\\f\\n\\r\\t\\v\\\\\\\"\\'\"",
       .expected = "\a\b\033\f\n\r\t\v\\\"'"},
      {.expr = "\"\\101\\102\"", .expected = "AB"},
      {.expr = "r\"a\\nb\\tc\"", .expected = "a\\nb\\tc"},
      {.expr = "r \"raw\"", .expected = "raw"},
      {.expr = "str(\"abc\")", .expected = "abc"},
      {.expr = "str(42)", .expected = "42"},
      {.expr = "str(-7)", .expected = "-7"},
      {.expr = "\"foo\" + \"bar\"", .expected = "foobar"},
      {.expr = "substr(1, 3, \"abcdef\")", .expected = "bcd"},
      {.expr = "ltrim(\"  abc  \")", .expected = "abc  "},
      {.expr = "rtrim(\"  abc  \")", .expected = "  abc"},
      {.expr = "trim(\"  abc  \")", .expected = "abc"},
      {.expr = "max(\"alpha\", \"beta\")", .expected = "beta"},
      {.expr = "min(\"alpha\", \"beta\")", .expected = "alpha"},
      {.expr = "regex(\"a([0-9]+)b\", \"xa42by\", 1)", .expected = "42"},
      // coda-expr.c:3605-3621 passes eval_string()'s non-NUL-terminated
      // substrname buffer to pcre2_substring_number_from_name; embed \000 so it
      // is clean under PCRE2 and MSan/ASan without triggering the matchstring
      // leak at coda-expr.c:3628.
      {.expr = "regex(\"a(?<num>[0-9]+)b\", \"xa42by\", \"num\\000\")",
       .expected = "42"},
      {.expr = "strtime(0)", .expected = "2000-01-01T00:00:00.000000"},
      {.expr = "strtime(90.0)", .expected = "2000-01-01T00:01:30.000000"},
      {.expr = "strtime(90, \"yyyy-MM-dd HH:mm:ss\")",
       .expected = "2000-01-01 00:01:30"},
      {.expr = "strtime(90.0, \"yyyy-MM-dd\")", .expected = "2000-01-01"},
      {.expr = "if(true, \"yes\", \"no\")", .expected = "yes"},
      {.expr = "if(false, \"yes\", \"no\")", .expected = "no"},
      {.expr = "with(i = 7, str(i))", .expected = "7"},
  };

  for (const StringCase& tc : kCases) {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        tc.expr, coda_expression_string, /*expected_is_constant=*/1, &expr));
    ScopedCodaString value;
    long length = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_string(
                                expr.get(), nullptr, value.OutParam(), &length))
        << "expr: " << tc.expr;
    ASSERT_EQ(static_cast<long>(tc.expected.size()), length)
        << "expr: " << tc.expr;
    if (length == 0) {
      EXPECT_EQ(nullptr, value.get()) << "expr: " << tc.expr;
    } else {
      ASSERT_NE(nullptr, value.get()) << "expr: " << tc.expr;
      EXPECT_EQ(tc.expected,
                std::string_view(value.get(), static_cast<std::size_t>(length)))
          << "expr: " << tc.expr;
    }
  }
}

TEST(CodaExprParserTest, ParsesAllNodeVoidAndNonConstantProductions) {
  struct NonConstCase {
    std::string_view expr;
    coda_expression_type expected_type;
  };
  constexpr NonConstCase kCases[] = {
      // rootnode, nonrootnode, and node productions.
      {.expr = "/", .expected_type = coda_expression_node},
      {.expr = ".", .expected_type = coda_expression_node},
      {.expr = ":", .expected_type = coda_expression_node},
      {.expr = "..", .expected_type = coda_expression_node},
      {.expr = "asciiline", .expected_type = coda_expression_node},
      {.expr = "/field", .expected_type = coda_expression_node},
      {.expr = "/{0}", .expected_type = coda_expression_node},
      {.expr = "./..", .expected_type = coda_expression_node},
      {.expr = "/a/..", .expected_type = coda_expression_node},
      {.expr = "./b", .expected_type = coda_expression_node},
      {.expr = "/a/b", .expected_type = coda_expression_node},
      {.expr = "./{1}", .expected_type = coda_expression_node},
      {.expr = "/a/{1}", .expected_type = coda_expression_node},
      {.expr = "[0]", .expected_type = coda_expression_node},
      {.expr = "/[0]", .expected_type = coda_expression_node},
      {.expr = "/arr[0]", .expected_type = coda_expression_node},
      {.expr = "@attr", .expected_type = coda_expression_node},
      {.expr = "@{0}", .expected_type = coda_expression_node},
      {.expr = "/@attr", .expected_type = coda_expression_node},
      {.expr = "/a@attr", .expected_type = coda_expression_node},
      {.expr = "/@{0}", .expected_type = coda_expression_node},
      {.expr = "/a@{0}", .expected_type = coda_expression_node},

      // voidexpr productions.
      {.expr = "$x = 1", .expected_type = coda_expression_void},
      {.expr = "$x[0] = 1", .expected_type = coda_expression_void},
      {.expr = "$x = 1; $y = 2", .expected_type = coda_expression_void},
      {.expr = "for i = 0 to 3 do $x[i] = i",
       .expected_type = coda_expression_void},
      {.expr = "for j = 0 to 6 step 2 do $x[j] = j",
       .expected_type = coda_expression_void},
      {.expr = "goto(/a)", .expected_type = coda_expression_void},
      {.expr = "with(k = 1, $x = k)", .expected_type = coda_expression_void},

      // Node/variable-dependent boolexpr productions.
      {.expr = "exists(/a)", .expected_type = coda_expression_boolean},
      {.expr = "exists(/a, int(.) == 1)",
       .expected_type = coda_expression_boolean},
      {.expr = "exists($v, $v == 1)", .expected_type = coda_expression_boolean},
      {.expr = "all(/a, int(.) > 0)", .expected_type = coda_expression_boolean},
      {.expr = "at(/a, int(.) == 1)", .expected_type = coda_expression_boolean},

      // Node/variable/product-dependent intexpr productions.
      {.expr = "int(/a)", .expected_type = coda_expression_integer},
      {.expr = "$x", .expected_type = coda_expression_integer},
      {.expr = "$x[0]", .expected_type = coda_expression_integer},
      {.expr = "max(/arr, int(.))", .expected_type = coda_expression_integer},
      {.expr = "min(/arr, int(.))", .expected_type = coda_expression_integer},
      {.expr = "numelements(/arr)", .expected_type = coda_expression_integer},
      {.expr = "numdims(/arr)", .expected_type = coda_expression_integer},
      {.expr = "dim(/arr, 0)", .expected_type = coda_expression_integer},
      {.expr = "count(/arr, int(.) > 0)",
       .expected_type = coda_expression_integer},
      {.expr = "add(/arr, int(.))", .expected_type = coda_expression_integer},
      {.expr = "length(/s)", .expected_type = coda_expression_integer},
      {.expr = "bitsize(/a)", .expected_type = coda_expression_integer},
      {.expr = "bytesize(/a)", .expected_type = coda_expression_integer},
      {.expr = "productversion()", .expected_type = coda_expression_integer},
      {.expr = "filesize()", .expected_type = coda_expression_integer},
      {.expr = "bitoffset(/a)", .expected_type = coda_expression_integer},
      {.expr = "byteoffset(/a)", .expected_type = coda_expression_integer},
      {.expr = "index(/arr[0])", .expected_type = coda_expression_integer},
      {.expr = "index(/arr, int(.) == 2)",
       .expected_type = coda_expression_integer},
      {.expr = "index($x, $x == 2)", .expected_type = coda_expression_integer},
      {.expr = "unboundindex(/arr, int(.) == 2)",
       .expected_type = coda_expression_integer},
      {.expr = "unboundindex(/arr, int(.) == 2, true)",
       .expected_type = coda_expression_integer},
      {.expr = "at(/a, int(.))", .expected_type = coda_expression_integer},

      // Node-dependent floatexpr productions.
      {.expr = "float(/f)", .expected_type = coda_expression_float},
      {.expr = "max(/arr, float(.))", .expected_type = coda_expression_float},
      {.expr = "min(/arr, float(.))", .expected_type = coda_expression_float},
      {.expr = "add(/arr, float(.))", .expected_type = coda_expression_float},
      {.expr = "at(/a, float(.))", .expected_type = coda_expression_float},

      // Node/product-dependent stringexpr productions.
      {.expr = "str(/s)", .expected_type = coda_expression_string},
      {.expr = "str(/s, 4)", .expected_type = coda_expression_string},
      {.expr = "bytes(/s)", .expected_type = coda_expression_string},
      {.expr = "bytes(/s, 4)", .expected_type = coda_expression_string},
      {.expr = "bytes(/s, 1, 3)", .expected_type = coda_expression_string},
      {.expr = "max(/arr, str(.))", .expected_type = coda_expression_string},
      {.expr = "min(/arr, str(.))", .expected_type = coda_expression_string},
      {.expr = "add(/arr, str(.))", .expected_type = coda_expression_string},
      {.expr = "productclass()", .expected_type = coda_expression_string},
      {.expr = "productformat()", .expected_type = coda_expression_string},
      {.expr = "producttype()", .expected_type = coda_expression_string},
      {.expr = "filename()", .expected_type = coda_expression_string},
      {.expr = "at(/a, str(.))", .expected_type = coda_expression_string},
  };

  for (const NonConstCase& tc : kCases) {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        tc.expr, tc.expected_type, /*expected_is_constant=*/0, &expr));
  }
}

TEST(CodaExprParserTest, ParsesAllIdentifiersAndReservedKeywordsAsFieldNames) {
  // Exercises identifier: NAME | INDEX_VAR | reserved_identifier across every
  // reserved_identifier production in coda-expr-parser.y.
  constexpr std::string_view kIdentifiers[] = {
      "field_1",
      "i",
      "j",
      "k",
      "r",
      "asciiline",
      "do",
      "for",
      "step",
      "to",
      "nan",
      "inf",
      "true",
      "false",
      "abs",
      "add",
      "all",
      "at",
      "bitoffset",
      "bool",
      "bytes",
      "byteoffset",
      "bytesize",
      "ceil",
      "count",
      "dim",
      "exists",
      "filename",
      "filesize",
      "float",
      "floor",
      "goto",
      "if",
      "index",
      "int",
      "isnan",
      "isinf",
      "isplusinf",
      "ismininf",
      "length",
      "ltrim",
      "numdims",
      "numelements",
      "max",
      "min",
      "productclass",
      "productformat",
      "producttype",
      "productversion",
      "regex",
      "round",
      "rtrim",
      "str",
      "strtime",
      "substr",
      "time",
      "trim",
      "unboundindex",
      "with",
  };

  for (const std::string_view id : kIdentifiers) {
    const std::string path = "/" + std::string(id);
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        path, coda_expression_node, /*expected_is_constant=*/0, &expr));
    EXPECT_EQ(path, PrintExpressionToString(expr.get()));
  }
}

TEST(CodaExprParserTest, EvaluatesParsedNodeAndArrayExpressionsOnXmlProduct) {
  constexpr std::string_view kXml =
      "<?xml version=\"1.0\"?>"
      "<root attr=\"hello\">"
      "<val>42</val>"
      "<items><item>10</item><item>20</item></items>"
      "</root>";
  TempFile xml_file("coda_expr_parser_sample.xml");
  ASSERT_TRUE(xml_file.WriteText(kXml));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // 1. Evaluate product metadata and string node expressions.
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "productformat()", coda_expression_string, 0, &expr));
    ScopedCodaString val;
    long len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_string(expr.get(), &cursor,
                                                        val.OutParam(), &len));
    ASSERT_NE(nullptr, val.get());
    EXPECT_EQ("xml",
              std::string_view(val.get(), static_cast<std::size_t>(len)));
  }
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "str(/root@attr)", coda_expression_string, 0, &expr));
    ScopedCodaString val;
    long len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_string(expr.get(), &cursor,
                                                        val.OutParam(), &len));
    ASSERT_NE(nullptr, val.get());
    EXPECT_EQ("hello",
              std::string_view(val.get(), static_cast<std::size_t>(len)));
  }

  // 2. Evaluate integer, array aggregation, and boolean predicate expressions.
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(
        ParseAndCheckMetadata("filesize()", coda_expression_integer, 0, &expr));
    int64_t size = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), &cursor, &size));
    EXPECT_EQ(static_cast<int64_t>(kXml.size()), size);
  }
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(
        ParseAndCheckMetadata("add(/root/items/item, int(str(.)))",
                              coda_expression_integer, 0, &expr));
    int64_t sum = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), &cursor, &sum));
    EXPECT_EQ(30, sum);
  }
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(
        ParseAndCheckMetadata("count(/root/items/item, int(str(.)) > 15)",
                              coda_expression_integer, 0, &expr));
    int64_t count = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), &cursor, &count));
    EXPECT_EQ(1, count);
  }
  {
    ScopedExpr expr;
    ASSERT_NO_FATAL_FAILURE(
        ParseAndCheckMetadata("all(/root/items/item, int(str(.)) >= 10)",
                              coda_expression_boolean, 0, &expr));
    int all_ok = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_bool(expr.get(), &cursor, &all_ok));
    EXPECT_EQ(1, all_ok);
  }

  // 3. Evaluate node navigation expressions directly on cursor.
  {
    ScopedExpr goto_item;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "/root/items/item[1]", coda_expression_node, 0, &goto_item));
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_node(goto_item.get(), &cursor));

    ScopedExpr read_here;
    ASSERT_NO_FATAL_FAILURE(ParseAndCheckMetadata(
        "int(str(.))", coda_expression_integer, 0, &read_here));
    int64_t item_val = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_integer(read_here.get(),
                                                         &cursor, &item_val));
    EXPECT_EQ(20, item_val);
  }
}

TEST(CodaExprParserTest, PrintAndEqualityReflectParsedAst) {
  struct PrintCase {
    std::string_view expr;
    std::string_view expected_printed;
  };
  constexpr PrintCase kPrintCases[] = {
      {.expr = "1 + 2 * 3", .expected_printed = "1 + 2 * 3"},
      {.expr = "(1 + 2) * 3", .expected_printed = "(1 + 2) * 3"},
      {.expr = "-(1 + 2)", .expected_printed = "-(1 + 2)"},
      {.expr = "true && false", .expected_printed = "true and false"},
      {.expr = "/a/b[0]@attr", .expected_printed = "/a/b[0]@attr"},
      {.expr = "for i = 0 to 2 step 1 do $x[i] = i",
       .expected_printed = "for i = 0 to 2 step 1 do $x[i] = i"},
      {.expr = "with(i = 1, i + 2)", .expected_printed = "with(i = 1, i + 2)"},
  };

  for (const PrintCase& tc : kPrintCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                std::string(tc.expr).c_str(), expr.OutParam()))
        << "expr: " << tc.expr;
    EXPECT_EQ(tc.expected_printed, PrintExpressionToString(expr.get()))
        << "expr: " << tc.expr;
  }

  // Unary '+' and identity casts int(int), float(float), str(str) unwrap in
  // the parser without wrapping in an extra operation node.
  struct EqualPair {
    std::string_view lhs;
    std::string_view rhs;
    int expected_equal;
  };
  constexpr EqualPair kEqualPairs[] = {
      {.lhs = "1 + 2", .rhs = "1 + 2", .expected_equal = 1},
      {.lhs = "+5", .rhs = "5", .expected_equal = 1},
      {.lhs = "+1.5", .rhs = "1.5", .expected_equal = 1},
      {.lhs = "int(42)", .rhs = "42", .expected_equal = 1},
      {.lhs = "float(1.5)", .rhs = "1.5", .expected_equal = 1},
      {.lhs = "str(\"abc\")", .rhs = "\"abc\"", .expected_equal = 1},
      {.lhs = "1 + 2", .rhs = "2 + 1", .expected_equal = 0},
      {.lhs = "(1 + 2) * 3", .rhs = "1 + 2 * 3", .expected_equal = 0},
      {.lhs = "r\"a\\nb\"", .rhs = "\"a\\nb\"", .expected_equal = 0},
  };

  for (const EqualPair& tc : kEqualPairs) {
    ScopedExpr lhs;
    ScopedExpr rhs;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                std::string(tc.lhs).c_str(), lhs.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                std::string(tc.rhs).c_str(), rhs.OutParam()));
    EXPECT_EQ(tc.expected_equal, coda_expression_is_equal(lhs.get(), rhs.get()))
        << tc.lhs << " vs " << tc.rhs;
  }
}

TEST(CodaExprParserTest, SyntaxAndSemanticErrorsAreRejectedCleanly) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Exercises yyerrlab, yysyntax_error across yycount 1..5, and yydestruct
  // cleanup for every token/nonterminal symbol type in the Bison stack and
  // lookahead slot.
  struct ErrorCase {
    std::string_view expr;
    int expected_errno;
  };
  constexpr ErrorCase kErrorCases[] = {
      // Empty & whitespace-only inputs.
      {.expr = "", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "   \t\n", .expected_errno = CODA_ERROR_EXPRESSION},
      // yysyntax_error expected-token count cases (1, 2, 3, 4, >5->1).
      {.expr = "=", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = ")", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "for i", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "r 123", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "$foo ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "true ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "goto(/a ?", .expected_errno = CODA_ERROR_EXPRESSION},
      // Lookahead token yydestruct paths (INT/FLOAT/STRING/NAME/INDEX_VAR).
      {.expr = "true 1", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "true 1.5", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "true \"s\"", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "true foo", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "true i", .expected_errno = CODA_ERROR_EXPRESSION},
      // Stack-pop yydestruct paths for all nonterminals.
      {.expr = "for i ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "$x = 1; ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "true && ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "1 + ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "1.5 + ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "\"a\" + ?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "/{?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "./?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "/a[?", .expected_errno = CODA_ERROR_EXPRESSION},
      {.expr = "/..", .expected_errno = CODA_ERROR_EXPRESSION},
      // Semantic failures returning NULL from coda_expression_new -> YYERROR.
      {.expr = "99999999999999999999",
       .expected_errno = CODA_ERROR_INVALID_FORMAT},
      {.expr = "\"\\q\"", .expected_errno = CODA_ERROR_INVALID_FORMAT},
      {.expr = "\"\\12\"", .expected_errno = CODA_ERROR_INVALID_FORMAT},
  };

  for (const ErrorCase& tc : kErrorCases) {
    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedExpr expr;
    EXPECT_EQ(-1, coda_expression_from_string(std::string(tc.expr).c_str(),
                                              expr.OutParam()))
        << "expr: " << tc.expr;
    EXPECT_EQ(nullptr, expr.get()) << "expr: " << tc.expr;
    EXPECT_EQ(tc.expected_errno, coda_errno) << "expr: " << tc.expr;
  }

  // Exceeding CODA_MAX_RECURSION_DEPTH (100) triggers YYERROR inside
  // coda_expression_new() and frees all intermediate AST nodes via yydestruct.
  {
    std::string deep_abs = "1";
    for (int i = 0; i < 105; ++i) {
      deep_abs = "abs(" + deep_abs + ")";
    }
    coda_set_error(CODA_SUCCESS, nullptr);
    ScopedExpr expr;
    EXPECT_EQ(-1,
              coda_expression_from_string(deep_abs.c_str(), expr.OutParam()));
    EXPECT_EQ(nullptr, expr.get());
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("maximum recursion depth"));
  }
}

TEST(CodaExprParserTest, HandlesBisonStackRelocationAndExhaustion) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Parenthesized integer expressions do not increment expr->recursion_depth,
  // so 250 nested parens exceed Bison's YYINITDEPTH (200) and trigger dynamic
  // stack reallocation (YYSTACK_RELOCATE) while remaining valid.
  {
    constexpr int kRelocateDepth = 250;
    const std::string parens = std::string(kRelocateDepth, '(') + "1" +
                               std::string(kRelocateDepth, ')');
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(parens.c_str(), expr.OutParam()));
    ASSERT_NE(nullptr, expr.get());
    int64_t val = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), nullptr, &val));
    EXPECT_EQ(1, val);
  }

  // Exceeding Bison's YYMAXDEPTH (10000) triggers yyexhaustedlab cleanly.
  {
    constexpr int kExhaustDepth = 10005;
    const std::string too_deep =
        std::string(kExhaustDepth, '(') + "1" + std::string(kExhaustDepth, ')');
    ScopedExpr expr;
    EXPECT_EQ(-1,
              coda_expression_from_string(too_deep.c_str(), expr.OutParam()));
    EXPECT_EQ(nullptr, expr.get());
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("memory exhausted"));
  }
}

// TODO(schwehr): 94.4% standalone llvm-cov line coverage in
// libcoda/coda-expr-parser.c (1773/1879, 7/7 functions); remaining lines are
// Bison YYENOMEM/yydestruct/YYERROR table fallback paths.

}  // namespace
}  // namespace stcorp_coda
