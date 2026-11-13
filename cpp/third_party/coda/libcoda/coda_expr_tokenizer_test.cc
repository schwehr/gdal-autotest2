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
// Tests the CODA expression Flex tokenizer (libcoda/coda-expr-tokenizer.c)
// both through coda_expression_from_string() and by directly exercising the
// Flex reentrant scanner, buffer-management, accessor, and allocator APIs.
//
// google3 build caveats:
//   - Generated #line directives pointing to "libcoda/coda-expr-tokenizer.l"
//     cause Blaze LCOV to emit 0 DA lines for coda-expr-tokenizer.c; standalone
//     llvm-cov measurement with #line directives stripped verifies a 100.0%
//     reachable ceiling (857/1001 = 85.6%, 144 missed: 77 dead YY_BREAK / '}'
//     lines after unconditional return in Flex rules 1..73, 45 dead Flex
//     skeleton lines from %option always-interactive / %option noyywrap /
//     inline backup / ECHO, 13 OOM lines, and 9 exit(2) yy_fatal_error lines).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-tokenizer.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-tokenizer.l
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-expr-parser.h

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

// coda-expr.h and coda-expr-parser.h lack extern "C" guards and define token
// macros (NOT, AND, OR, FOR, DO, STEP, TO, EQUAL, NAME, etc.), so include
// them after all C++ / Abseil / GoogleTest / util headers.
extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-expr-parser.h"
#include "third_party/stcorp_coda/libcoda/coda-expr.h"

struct yy_buffer_state;
typedef struct yy_buffer_state* YY_BUFFER_STATE;
typedef void* yyscan_t;

int coda_expression_lex_init(yyscan_t* ptr_yy_globals);
int coda_expression_lex_init_extra(void* yy_user_defined,
                                   yyscan_t* ptr_yy_globals);
int coda_expression_lex_destroy(yyscan_t yyscanner);
int coda_expression_lex(YYSTYPE* yylval_param, yyscan_t yyscanner);
void coda_expression_restart(FILE* input_file, yyscan_t yyscanner);
void coda_expression__switch_to_buffer(YY_BUFFER_STATE new_buffer,
                                       yyscan_t yyscanner);
YY_BUFFER_STATE coda_expression__create_buffer(FILE* file, int size,
                                               yyscan_t yyscanner);
void coda_expression__delete_buffer(YY_BUFFER_STATE b, yyscan_t yyscanner);
void coda_expression__flush_buffer(YY_BUFFER_STATE b, yyscan_t yyscanner);
void coda_expression_push_buffer_state(YY_BUFFER_STATE new_buffer,
                                       yyscan_t yyscanner);
void coda_expression_pop_buffer_state(yyscan_t yyscanner);
YY_BUFFER_STATE coda_expression__scan_buffer(char* base, size_t size,
                                             yyscan_t yyscanner);
YY_BUFFER_STATE coda_expression__scan_string(const char* yy_str,
                                             yyscan_t yyscanner);
YY_BUFFER_STATE coda_expression__scan_bytes(const char* yybytes, int len,
                                            yyscan_t yyscanner);
void* coda_expression_get_extra(yyscan_t yyscanner);
void coda_expression_set_extra(void* user_defined, yyscan_t yyscanner);
int coda_expression_get_lineno(yyscan_t yyscanner);
void coda_expression_set_lineno(int line_number, yyscan_t yyscanner);
int coda_expression_get_column(yyscan_t yyscanner);
void coda_expression_set_column(int column_no, yyscan_t yyscanner);
FILE* coda_expression_get_in(yyscan_t yyscanner);
void coda_expression_set_in(FILE* in_str, yyscan_t yyscanner);
FILE* coda_expression_get_out(yyscan_t yyscanner);
void coda_expression_set_out(FILE* out_str, yyscan_t yyscanner);
int coda_expression_get_leng(yyscan_t yyscanner);
char* coda_expression_get_text(yyscan_t yyscanner);
int coda_expression_get_debug(yyscan_t yyscanner);
void coda_expression_set_debug(int bdebug, yyscan_t yyscanner);
YYSTYPE* coda_expression_get_lval(yyscan_t yyscanner);
void coda_expression_set_lval(YYSTYPE* yylval_param, yyscan_t yyscanner);
void* coda_expression_alloc(size_t size, yyscan_t yyscanner);
void* coda_expression_realloc(void* ptr, size_t size, yyscan_t yyscanner);
void coda_expression_free(void* ptr, yyscan_t yyscanner);
}  // extern "C"

namespace stcorp_coda {
namespace {

struct FileCloser {
  void operator()(std::FILE* f) const {
    if (f != nullptr) {
      std::fclose(f);
    }
  }
};

using ScopedFile = std::unique_ptr<std::FILE, FileCloser>;

// RAII wrapper for Flex reentrant scanner state.
class ScopedExprScanner {
 public:
  ScopedExprScanner() = default;
  ~ScopedExprScanner() { Reset(); }

  ScopedExprScanner(const ScopedExprScanner&) = delete;
  ScopedExprScanner& operator=(const ScopedExprScanner&) = delete;

  int Init() {
    Reset();
    return coda_expression_lex_init(&scanner_);
  }

  int InitExtra(void* user_defined) {
    Reset();
    return coda_expression_lex_init_extra(user_defined, &scanner_);
  }

  void Reset() {
    if (scanner_ != nullptr) {
      coda_expression_lex_destroy(scanner_);
      scanner_ = nullptr;
    }
  }

  yyscan_t get() const { return scanner_; }

 private:
  yyscan_t scanner_ = nullptr;
};

// Calls coda_expression_lex() and frees yylval.stringval for string-carrying
// tokens so callers never leak strdup/malloc allocations.
int NextToken(yyscan_t scanner, std::string* out_value = nullptr) {
  YYSTYPE lval{};
  const int tok = coda_expression_lex(&lval, scanner);
  if (tok == STRING_VALUE || tok == NAME || tok == INDEX_VAR ||
      tok == INT_VALUE || tok == FLOAT_VALUE) {
    const absl::Cleanup free_val = [&] { std::free(lval.stringval); };
    if (out_value != nullptr && lval.stringval != nullptr) {
      *out_value = lval.stringval;
    }
  } else if (out_value != nullptr) {
    out_value->clear();
  }
  return tok;
}

TEST(CodaExprTokenizerTest, FromStringParsesStringLiteralsAndEscapes) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Empty string constant frees buffer and returns length 0.
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("\"\"", expr.OutParam()));
    coda_expression_type expr_type = coda_expression_void;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &expr_type));
    EXPECT_EQ(coda_expression_string, expr_type);

    ScopedCodaString value;
    long length = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_string(expr.get(), nullptr, value.OutParam(),
                                          &length));
    EXPECT_EQ(0L, length);
    EXPECT_EQ(nullptr, value.get());
  }

  // 2. Normal, multiline, escaped, and raw string literals.
  struct StringCase {
    const char* expr_text;
    std::string_view expected;
  };
  constexpr StringCase kStringCases[] = {
      {.expr_text = "\"hello world\"", .expected = "hello world"},
      {.expr_text = "\"line1\nline2\"", .expected = "line1\nline2"},
      {.expr_text = R"("a\"b\\c\nd\te\rf\vg\ah\bi\ej\f\'k")",
       .expected = "a\"b\\c\nd\te\rf\vg\ah\bi\033j\f'k"},
      {.expr_text = R"("\101\102\060")", .expected = "AB0"},
      {.expr_text = R"(r"a\nb\\c")", .expected = R"(a\nb\\c)"},
  };
  for (const StringCase& tc : kStringCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    ScopedCodaString value;
    long length = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_string(
                                expr.get(), nullptr, value.OutParam(), &length))
        << tc.expr_text;
    ASSERT_NE(nullptr, value.get()) << tc.expr_text;
    EXPECT_EQ(static_cast<long>(tc.expected.size()), length) << tc.expr_text;
    EXPECT_EQ(tc.expected,
              std::string_view(value.get(), static_cast<size_t>(length)));
  }

  // 3. Invalid escape sequence is rejected by string_constant_new().
  {
    ScopedExpr expr;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_expression_from_string(R"("\q")", expr.OutParam()));
    EXPECT_EQ(nullptr, expr.get());
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);
  }

  // 4. Unterminated string literal falls back to '"' token and fails parsing.
  {
    ScopedExpr expr;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_expression_from_string("\"unterminated", expr.OutParam()));
    EXPECT_EQ(nullptr, expr.get());
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  }
}

TEST(CodaExprTokenizerTest, FromStringParsesIntegersAndFloats) {
  struct IntCase {
    const char* expr_text;
    int64_t expected;
  };
  constexpr IntCase kIntCases[] = {
      {.expr_text = "0", .expected = 0},
      {.expr_text = "42", .expected = 42},
      {.expr_text = "-17", .expected = -17},
      {.expr_text = "9223372036854775807", .expected = INT64_MAX},
  };
  for (const IntCase& tc : kIntCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    coda_expression_type expr_type = coda_expression_void;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &expr_type));
    EXPECT_EQ(coda_expression_integer, expr_type);

    int64_t actual = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), nullptr, &actual));
    EXPECT_EQ(tc.expected, actual) << tc.expr_text;
  }

  struct FloatCase {
    const char* expr_text;
    double expected;
  };
  constexpr FloatCase kFloatCases[] = {
      {.expr_text = "1.", .expected = 1.0},
      {.expr_text = ".5", .expected = 0.5},
      {.expr_text = "1.5", .expected = 1.5},
      {.expr_text = "-3.5", .expected = -3.5},
      {.expr_text = "1e5", .expected = 1e5},
      {.expr_text = "1E+5", .expected = 1e5},
      {.expr_text = "1e-5", .expected = 1e-5},
      {.expr_text = "2.e2", .expected = 200.0},
      {.expr_text = "2.E+2", .expected = 200.0},
      {.expr_text = "2.e-2", .expected = 0.02},
      {.expr_text = ".25e2", .expected = 25.0},
      {.expr_text = ".25E+2", .expected = 25.0},
      {.expr_text = ".25e-2", .expected = 0.0025},
      {.expr_text = "1.25e2", .expected = 125.0},
      {.expr_text = "1.25E-2", .expected = 0.0125},
  };
  for (const FloatCase& tc : kFloatCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    coda_expression_type expr_type = coda_expression_void;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &expr_type));
    EXPECT_EQ(coda_expression_float, expr_type);

    double actual = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &actual));
    EXPECT_DOUBLE_EQ(tc.expected, actual) << tc.expr_text;
  }
}

TEST(CodaExprTokenizerTest, FromStringParsesOperatorsAndPunctuation) {
  struct BoolCase {
    const char* expr_text;
    int expected;
  };
  constexpr BoolCase kBoolCases[] = {
      {.expr_text = "2 >= 2", .expected = 1},
      {.expr_text = "1 >= 2", .expected = 0},
      {.expr_text = "2 <= 2", .expected = 1},
      {.expr_text = "3 <= 2", .expected = 0},
      {.expr_text = "2 == 2", .expected = 1},
      {.expr_text = "2 != 3", .expected = 1},
      {.expr_text = "1 < 2", .expected = 1},
      {.expr_text = "2 > 1", .expected = 1},
      {.expr_text = "true && false", .expected = 0},
      {.expr_text = "true || false", .expected = 1},
      {.expr_text = "!false", .expected = 1},
  };
  for (const BoolCase& tc : kBoolCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    int actual = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_bool(expr.get(), nullptr, &actual));
    EXPECT_EQ(tc.expected, actual) << tc.expr_text;
  }

  struct IntOpCase {
    const char* expr_text;
    int64_t expected;
  };
  constexpr IntOpCase kIntOpCases[] = {
      {.expr_text = "6 & 3", .expected = 2},
      {.expr_text = "4 | 2", .expected = 6},
      {.expr_text = "(1 + 2) * 3 - 4 / 2 + 7 % 3", .expected = 8},
      {.expr_text = "+5", .expected = 5},
  };
  for (const IntOpCase& tc : kIntOpCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    int64_t actual = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), nullptr, &actual));
    EXPECT_EQ(tc.expected, actual) << tc.expr_text;
  }

  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("2 ^ 3", expr.OutParam()));
    double actual = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &actual));
    EXPECT_DOUBLE_EQ(8.0, actual);
  }

  struct SyntaxTypeCase {
    const char* expr_text;
    coda_expression_type expected_type;
  };
  constexpr SyntaxTypeCase kPunctCases[] = {
      {.expr_text = ".", .expected_type = coda_expression_node},
      {.expr_text = "..", .expected_type = coda_expression_node},
      {.expr_text = "./..", .expected_type = coda_expression_node},
      {.expr_text = ":", .expected_type = coda_expression_node},
      {.expr_text = "/{0}", .expected_type = coda_expression_node},
      {.expr_text = "@attr_name", .expected_type = coda_expression_node},
      {.expr_text = "@{0}", .expected_type = coda_expression_node},
      {.expr_text = "[0]", .expected_type = coda_expression_node},
      {.expr_text = "$x = 1; $x[0] = 2", .expected_type = coda_expression_void},
      {.expr_text = "if(true, 1, 2)", .expected_type = coda_expression_integer},
  };
  for (const SyntaxTypeCase& tc : kPunctCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    coda_expression_type actual_type = coda_expression_void;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &actual_type));
    EXPECT_EQ(tc.expected_type, actual_type) << tc.expr_text;
  }
}

TEST(CodaExprTokenizerTest,
     FromStringParsesAllKeywordsIndexVarsAndIdentifiers) {
  struct KeywordExprCase {
    const char* expr_text;
    coda_expression_type expected_type;
  };
  // Covers all 56 keyword rules in coda-expr-tokenizer.l.
  constexpr KeywordExprCase kKeywordExprCases[] = {
      {.expr_text = R"(r"raw\n")", .expected_type = coda_expression_string},
      {.expr_text = "asciiline", .expected_type = coda_expression_node},
      {.expr_text = "for i = 1 to 5 step 2 do $x = i",
       .expected_type = coda_expression_void},
      {.expr_text = "nan", .expected_type = coda_expression_float},
      {.expr_text = "inf", .expected_type = coda_expression_float},
      {.expr_text = "true", .expected_type = coda_expression_boolean},
      {.expr_text = "false", .expected_type = coda_expression_boolean},
      {.expr_text = "abs(-7)", .expected_type = coda_expression_integer},
      {.expr_text = "add(., 0)", .expected_type = coda_expression_integer},
      {.expr_text = "all(., true)", .expected_type = coda_expression_boolean},
      {.expr_text = "at(., 1)", .expected_type = coda_expression_integer},
      {.expr_text = "bitoffset(.)", .expected_type = coda_expression_integer},
      {.expr_text = "bitsize(.)", .expected_type = coda_expression_integer},
      // FUNC_BOOL is valid as a reserved_identifier in node paths.
      {.expr_text = "/bool", .expected_type = coda_expression_node},
      {.expr_text = "bytes(.)", .expected_type = coda_expression_string},
      {.expr_text = "byteoffset(.)", .expected_type = coda_expression_integer},
      {.expr_text = "bytesize(.)", .expected_type = coda_expression_integer},
      {.expr_text = "ceil(1.25)", .expected_type = coda_expression_float},
      {.expr_text = "count(., true)", .expected_type = coda_expression_integer},
      {.expr_text = "dim(., 0)", .expected_type = coda_expression_integer},
      {.expr_text = "exists(.)", .expected_type = coda_expression_boolean},
      {.expr_text = "filename()", .expected_type = coda_expression_string},
      {.expr_text = "filesize()", .expected_type = coda_expression_integer},
      {.expr_text = "float(3)", .expected_type = coda_expression_float},
      {.expr_text = "floor(1.75)", .expected_type = coda_expression_float},
      {.expr_text = "goto(.)", .expected_type = coda_expression_void},
      {.expr_text = "if(true, 10, 20)",
       .expected_type = coda_expression_integer},
      {.expr_text = "index(.)", .expected_type = coda_expression_integer},
      {.expr_text = "int(\"42\")", .expected_type = coda_expression_integer},
      {.expr_text = "isnan(nan)", .expected_type = coda_expression_boolean},
      {.expr_text = "isinf(inf)", .expected_type = coda_expression_boolean},
      {.expr_text = "isplusinf(inf)", .expected_type = coda_expression_boolean},
      {.expr_text = "ismininf(-inf)", .expected_type = coda_expression_boolean},
      {.expr_text = "length(\"hello\")",
       .expected_type = coda_expression_integer},
      {.expr_text = "ltrim(\"  hi\")", .expected_type = coda_expression_string},
      {.expr_text = "numdims(.)", .expected_type = coda_expression_integer},
      {.expr_text = "numelements(.)", .expected_type = coda_expression_integer},
      {.expr_text = "max(3, 9)", .expected_type = coda_expression_integer},
      {.expr_text = "min(3, 9)", .expected_type = coda_expression_integer},
      {.expr_text = "productclass()", .expected_type = coda_expression_string},
      {.expr_text = "productformat()", .expected_type = coda_expression_string},
      {.expr_text = "producttype()", .expected_type = coda_expression_string},
      {.expr_text = "productversion()",
       .expected_type = coda_expression_integer},
      {.expr_text = "regex(\"a+\", \"aaa\")",
       .expected_type = coda_expression_boolean},
      {.expr_text = "round(2.6)", .expected_type = coda_expression_float},
      {.expr_text = "rtrim(\"hi  \")", .expected_type = coda_expression_string},
      {.expr_text = "str(123)", .expected_type = coda_expression_string},
      {.expr_text = "strtime(0)", .expected_type = coda_expression_string},
      {.expr_text = "substr(1, 3, \"abcdef\")",
       .expected_type = coda_expression_string},
      {.expr_text = "time(\"2000-01-01\", \"yyyy-MM-dd\")",
       .expected_type = coda_expression_float},
      {.expr_text = "trim(\"  hi  \")",
       .expected_type = coda_expression_string},
      {.expr_text = "unboundindex(., true)",
       .expected_type = coda_expression_integer},
      {.expr_text = "with(i = 2, i * 5)",
       .expected_type = coda_expression_integer},
  };
  for (const KeywordExprCase& tc : kKeywordExprCases) {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(tc.expr_text, expr.OutParam()))
        << tc.expr_text;
    coda_expression_type actual_type = coda_expression_void;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &actual_type));
    EXPECT_EQ(tc.expected_type, actual_type) << tc.expr_text;
  }

  // Verify index variables i, j, k and constant keyword evaluations.
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(
                  "with(i = 1, with(j = 2, with(k = 3, i + j + k)))",
                  expr.OutParam()));
    int64_t actual = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), nullptr, &actual));
    EXPECT_EQ(6, actual);
  }
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(
                  "if(regex(\"a+\", \"aaa\"), abs(-7), 0)", expr.OutParam()));
    int64_t actual = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_integer(expr.get(), nullptr, &actual));
    EXPECT_EQ(7, actual);
  }
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("substr(1, 3, trim(\"  abcdef  \"))",
                                          expr.OutParam()));
    ScopedCodaString value;
    long length = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_string(expr.get(), nullptr, value.OutParam(),
                                          &length));
    ASSERT_NE(nullptr, value.get());
    EXPECT_EQ(3L, length);
    EXPECT_STREQ("bcd", value.get());
  }
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("nan", expr.OutParam()));
    double actual = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &actual));
    EXPECT_TRUE(std::isnan(actual));
  }
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("inf", expr.OutParam()));
    double actual = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_float(expr.get(), nullptr, &actual));
    EXPECT_TRUE(std::isinf(actual));
    EXPECT_GT(actual, 0.0);
  }

  // Verify identifiers starting with index/keyword prefixes or uppercase stay
  // NAME tokens rather than keywords.
  {
    ScopedExpr expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(
                  "/i0/j_1/k2/r2/true_val/format/TRUE/NAN/Alpha_99",
                  expr.OutParam()));
    coda_expression_type actual_type = coda_expression_void;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_get_type(expr.get(), &actual_type));
    EXPECT_EQ(coda_expression_node, actual_type);
  }
}

TEST(CodaExprTokenizerTest, FromStringRejectsUnknownTokensAndInvalidInputs) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedExpr expr;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_from_string(nullptr, expr.OutParam()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_from_string("1", nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  constexpr const char* kBadExpressions[] = {
      "", "   \t\n\r\v\f  ", "?", "~", "#", "'abc'", "1 ? 2", "123e+",
  };
  for (const char* bad : kBadExpressions) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_expression_from_string(bad, expr.OutParam())) << bad;
    EXPECT_EQ(nullptr, expr.get()) << bad;
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno) << bad;
  }
}

TEST(CodaExprTokenizerTest, DirectLexTokenizesAllTokenClasses) {
  ScopedExprScanner scanner;
  ASSERT_EQ(0, scanner.Init());

  struct TokenCase {
    const char* text;
    int expected_token;
    const char* expected_str;  // non-null for stringval-carrying tokens.
  };
  constexpr TokenCase kTokenCases[] = {
      // Multi-char operators
      {">=", GREATER_EQUAL, nullptr},
      {"<=", LESS_EQUAL, nullptr},
      {"==", EQUAL, nullptr},
      {"!=", NOT_EQUAL, nullptr},
      {"&", AND, nullptr},
      {"|", OR, nullptr},
      {"&&", LOGICAL_AND, nullptr},
      {"||", LOGICAL_OR, nullptr},
      {"!", NOT, nullptr},
      {"..", GOTO_PARENT, nullptr},
      // All 56 keywords
      {"r", RAW_PREFIX, nullptr},
      {"asciiline", ASCIILINE, nullptr},
      {"do", DO, nullptr},
      {"for", FOR, nullptr},
      {"step", STEP, nullptr},
      {"to", TO, nullptr},
      {"nan", NAN_VALUE, nullptr},
      {"inf", INF_VALUE, nullptr},
      {"true", TRUE_VALUE, nullptr},
      {"false", FALSE_VALUE, nullptr},
      {"abs", FUNC_ABS, nullptr},
      {"add", FUNC_ADD, nullptr},
      {"all", FUNC_ALL, nullptr},
      {"at", FUNC_AT, nullptr},
      {"bitoffset", FUNC_BITOFFSET, nullptr},
      {"bitsize", FUNC_BITSIZE, nullptr},
      {"bool", FUNC_BOOL, nullptr},
      {"bytes", FUNC_BYTES, nullptr},
      {"byteoffset", FUNC_BYTEOFFSET, nullptr},
      {"bytesize", FUNC_BYTESIZE, nullptr},
      {"ceil", FUNC_CEIL, nullptr},
      {"count", FUNC_COUNT, nullptr},
      {"dim", FUNC_DIM, nullptr},
      {"exists", FUNC_EXISTS, nullptr},
      {"filename", FUNC_FILENAME, nullptr},
      {"filesize", FUNC_FILESIZE, nullptr},
      {"float", FUNC_FLOAT, nullptr},
      {"floor", FUNC_FLOOR, nullptr},
      {"goto", FUNC_GOTO, nullptr},
      {"if", FUNC_IF, nullptr},
      {"index", FUNC_INDEX, nullptr},
      {"int", FUNC_INT, nullptr},
      {"isnan", FUNC_ISNAN, nullptr},
      {"isinf", FUNC_ISINF, nullptr},
      {"isplusinf", FUNC_ISPLUSINF, nullptr},
      {"ismininf", FUNC_ISMININF, nullptr},
      {"length", FUNC_LENGTH, nullptr},
      {"ltrim", FUNC_LTRIM, nullptr},
      {"numdims", FUNC_NUMDIMS, nullptr},
      {"numelements", FUNC_NUMELEMENTS, nullptr},
      {"max", FUNC_MAX, nullptr},
      {"min", FUNC_MIN, nullptr},
      {"productclass", FUNC_PRODUCTCLASS, nullptr},
      {"productformat", FUNC_PRODUCTFORMAT, nullptr},
      {"producttype", FUNC_PRODUCTTYPE, nullptr},
      {"productversion", FUNC_PRODUCTVERSION, nullptr},
      {"regex", FUNC_REGEX, nullptr},
      {"round", FUNC_ROUND, nullptr},
      {"rtrim", FUNC_RTRIM, nullptr},
      {"str", FUNC_STR, nullptr},
      {"strtime", FUNC_STRTIME, nullptr},
      {"substr", FUNC_SUBSTR, nullptr},
      {"time", FUNC_TIME, nullptr},
      {"trim", FUNC_TRIM, nullptr},
      {"unboundindex", FUNC_UNBOUNDINDEX, nullptr},
      {"with", FUNC_WITH, nullptr},
      // Index variables
      {"i", INDEX_VAR, "i"},
      {"j", INDEX_VAR, "j"},
      {"k", INDEX_VAR, "k"},
      // Identifiers
      {"alpha_1", NAME, "alpha_1"},
      {"i0", NAME, "i0"},
      {"r2", NAME, "r2"},
      {"TRUE", NAME, "TRUE"},
      // Integers & floats
      {"0", INT_VALUE, "0"},
      {"98765", INT_VALUE, "98765"},
      {"1.", FLOAT_VALUE, "1."},
      {".25", FLOAT_VALUE, ".25"},
      {"3.14", FLOAT_VALUE, "3.14"},
      {"4e2", FLOAT_VALUE, "4e2"},
      {"4E+2", FLOAT_VALUE, "4E+2"},
      {"4e-2", FLOAT_VALUE, "4e-2"},
      {"1.5e-3", FLOAT_VALUE, "1.5e-3"},
      // String literals (note tokenizer keeps escapes verbatim inside quotes)
      {"\"\"", STRING_VALUE, ""},
      {R"("a\"b\\c\nd")", STRING_VALUE, R"(a\"b\\c\nd)"},
      {"\"m1\nm2\"", STRING_VALUE, "m1\nm2"},
      // Single-char operators & unknown tokens
      {"+", '+', nullptr},
      {"-", '-', nullptr},
      {"*", '*', nullptr},
      {"/", '/', nullptr},
      {"%", '%', nullptr},
      {"^", '^', nullptr},
      {"<", '<', nullptr},
      {">", '>', nullptr},
      {"(", '(', nullptr},
      {")", ')', nullptr},
      {"[", '[', nullptr},
      {"]", ']', nullptr},
      {"{", '{', nullptr},
      {"}", '}', nullptr},
      {",", ',', nullptr},
      {";", ';', nullptr},
      {":", ':', nullptr},
      {".", '.', nullptr},
      {"$", '$', nullptr},
      {"@", '@', nullptr},
      {"=", '=', nullptr},
      {"?", '?', nullptr},
      {"~", '~', nullptr},
      {"#", '#', nullptr},
  };

  // Build one combined input using all whitespace chars so every token is
  // scanned in sequence without trailing whitespace (also exercises EOB last
  // match on yy_fill_buffer == 0).
  constexpr char kWhitespace[] = {' ', '\t', '\n', '\r', '\v', '\f'};
  std::string stream;
  size_t idx = 0;
  for (const TokenCase& tc : kTokenCases) {
    if (idx > 0) {
      stream.push_back(kWhitespace[idx % sizeof(kWhitespace)]);
    }
    stream.append(tc.text);
    ++idx;
  }

  YY_BUFFER_STATE buf =
      coda_expression__scan_string(stream.c_str(), scanner.get());
  ASSERT_NE(nullptr, buf);

  std::string actual_str;
  for (const TokenCase& tc : kTokenCases) {
    const int tok = NextToken(scanner.get(), &actual_str);
    EXPECT_EQ(tc.expected_token, tok) << tc.text;
    EXPECT_STREQ(tc.text, coda_expression_get_text(scanner.get()));
    EXPECT_EQ(static_cast<int>(std::strlen(tc.text)),
              coda_expression_get_leng(scanner.get()));
    if (tc.expected_str != nullptr) {
      EXPECT_EQ(tc.expected_str, actual_str) << tc.text;
    }
  }
  EXPECT_EQ(0, NextToken(scanner.get(), &actual_str));

  // Verify DFA backup (yy_act == 0) when a partial float exponent or
  // unterminated string fails mid-match and falls back to earlier accepting
  // states. Delete `buf` before replacing it via coda_expression__scan_string.
  coda_expression__delete_buffer(buf, scanner.get());
  YY_BUFFER_STATE backup_buf =
      coda_expression__scan_string("123e+ \"unterminated", scanner.get());
  ASSERT_NE(nullptr, backup_buf);
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("123", actual_str);
  EXPECT_EQ(NAME, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("e", actual_str);
  EXPECT_EQ('+', NextToken(scanner.get()));
  EXPECT_EQ('"', NextToken(scanner.get()));
  EXPECT_EQ(NAME, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("unterminated", actual_str);
  EXPECT_EQ(0, NextToken(scanner.get()));
}

TEST(CodaExprTokenizerTest, DirectLexScanBytesHandlesEmbeddedNul) {
  ScopedExprScanner scanner;
  ASSERT_EQ(0, scanner.Init());

  // 1. Embedded NUL inside a string literal exercises yy_try_NUL_trans()
  // continuing a match (non-zero next state via yy_def/yy_meta).
  constexpr char kNulInString[] = "\"ab\0cd\" 9";
  YY_BUFFER_STATE buf1 = coda_expression__scan_bytes(
      kNulInString, static_cast<int>(sizeof(kNulInString) - 1), scanner.get());
  ASSERT_NE(nullptr, buf1);

  YYSTYPE lval{};
  ASSERT_EQ(STRING_VALUE, coda_expression_lex(&lval, scanner.get()));
  const absl::Cleanup free_str = [&] { std::free(lval.stringval); };
  EXPECT_EQ(7, coda_expression_get_leng(scanner.get()));
  ASSERT_NE(nullptr, lval.stringval);
  EXPECT_EQ(0, std::memcmp(lval.stringval, "ab\0cd", 5));

  std::string actual_str;
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("9", actual_str);
  EXPECT_EQ(0, NextToken(scanner.get()));

  // 2. Embedded NUL right after an integer exercises yy_get_previous_state()
  // with an accepting state and yy_try_NUL_trans() jamming (returning 0).
  // Delete `buf1` before replacing it via coda_expression__scan_bytes.
  coda_expression__delete_buffer(buf1, scanner.get());
  constexpr char kNulAfterInt[] = "123\0x";
  YY_BUFFER_STATE buf2 = coda_expression__scan_bytes(
      kNulAfterInt, static_cast<int>(sizeof(kNulAfterInt) - 1), scanner.get());
  ASSERT_NE(nullptr, buf2);
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("123", actual_str);
  EXPECT_EQ(0, NextToken(scanner.get()));
}

TEST(CodaExprTokenizerTest, DirectLexScanBufferValidatesAndScansMutableBuffer) {
  std::vector<char> mutable_buf = {'1', '2', '+', '3', '\0', '\0'};
  ScopedExprScanner scanner;
  ASSERT_EQ(0, scanner.Init());

  char too_small[1] = {'\0'};
  EXPECT_EQ(nullptr, coda_expression__scan_buffer(too_small, 1, scanner.get()));

  char missing_double_nul[4] = {'1', '+', '\0', 'x'};
  EXPECT_EQ(nullptr,
            coda_expression__scan_buffer(missing_double_nul, 4, scanner.get()));

  char missing_penultimate_nul[4] = {'1', '+', '2', '\0'};
  EXPECT_EQ(nullptr, coda_expression__scan_buffer(missing_penultimate_nul, 4,
                                                  scanner.get()));

  YY_BUFFER_STATE buf = coda_expression__scan_buffer(
      mutable_buf.data(), mutable_buf.size(), scanner.get());
  ASSERT_NE(nullptr, buf);
  const absl::Cleanup delete_buf = [&] {
    coda_expression__delete_buffer(buf, scanner.get());
  };

  std::string actual_str;
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("12", actual_str);
  EXPECT_EQ('+', NextToken(scanner.get()));
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("3", actual_str);
  EXPECT_EQ(0, NextToken(scanner.get()));
}

TEST(CodaExprTokenizerTest, DirectLexFileStreamAndBufferGrowth) {
  TempFile file1("coda_expr_tok_stream1.txt");
  ASSERT_TRUE(file1.WriteText("12 + 34\n56"));

  TempFile file2("coda_expr_tok_stream2.txt");
  // Exceeds YY_READ_BUF_SIZE (8192) to exercise EOB_ACT_CONTINUE_SCAN on FILE*.
  const std::string long_ident(9000, 'a');
  ASSERT_TRUE(file2.WriteText(long_ident));

  TempFile file3("coda_expr_tok_stream3.txt");
  // Exceeds a 16-byte buffer to exercise yy_get_next_buffer() buffer growth.
  const std::string medium_ident(120, 'b');
  ASSERT_TRUE(file3.WriteText(medium_ident));

  TempFile file4("coda_expr_tok_stream4.txt");
  ASSERT_TRUE(file4.WriteText("789"));

  ScopedFile fp1(std::fopen(file1.c_str(), "rb"));
  ScopedFile fp2(std::fopen(file2.c_str(), "rb"));
  ScopedFile fp3(std::fopen(file3.c_str(), "rb"));
  ScopedFile fp4(std::fopen(file4.c_str(), "rb"));
  ASSERT_NE(nullptr, fp1);
  ASSERT_NE(nullptr, fp2);
  ASSERT_NE(nullptr, fp3);
  ASSERT_NE(nullptr, fp4);

  // 1. Lex directly from FILE* without pre-creating a buffer so yylex() creates
  // the default YY_BUF_SIZE buffer and reads through interactive YY_INPUT.
  {
    ScopedExprScanner scanner;
    ASSERT_EQ(0, scanner.Init());
    coda_expression_set_in(fp1.get(), scanner.get());

    std::string actual_str;
    EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
    EXPECT_EQ("12", actual_str);
    EXPECT_EQ('+', NextToken(scanner.get()));
    EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
    EXPECT_EQ("34", actual_str);
    // "56" ends at EOF without a trailing newline (EOB_ACT_LAST_MATCH ->
    // YY_BUFFER_EOF_PENDING -> EOB_ACT_END_OF_FILE).
    EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
    EXPECT_EQ("56", actual_str);
    EXPECT_EQ(0, NextToken(scanner.get()));

    // 2. Restart onto a >8192-byte token stream using the existing buffer.
    coda_expression_restart(fp2.get(), scanner.get());
    EXPECT_EQ(NAME, NextToken(scanner.get(), &actual_str));
    EXPECT_EQ(long_ident, actual_str);
    EXPECT_EQ(0, NextToken(scanner.get()));
  }

  // 3. Use a small 16-byte buffer in a fresh scanner so yy_get_next_buffer()
  // doubles it via coda_expression_realloc() without leaking any prior buffer.
  {
    ScopedExprScanner scanner;
    ASSERT_EQ(0, scanner.Init());
    YY_BUFFER_STATE small_buf =
        coda_expression__create_buffer(fp3.get(), 16, scanner.get());
    ASSERT_NE(nullptr, small_buf);
    coda_expression__switch_to_buffer(small_buf, scanner.get());

    std::string actual_str;
    EXPECT_EQ(NAME, NextToken(scanner.get(), &actual_str));
    EXPECT_EQ(medium_ident, actual_str);
    EXPECT_EQ(0, NextToken(scanner.get()));
  }

  // 4. Restart on a fresh scanner that has no current buffer yet.
  {
    ScopedExprScanner scanner;
    ASSERT_EQ(0, scanner.Init());
    coda_expression_restart(fp4.get(), scanner.get());
    std::string actual_str;
    EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
    EXPECT_EQ("789", actual_str);
    EXPECT_EQ(0, NextToken(scanner.get()));
  }
}

TEST(CodaExprTokenizerTest, BufferStackAndManagementApis) {
  TempFile f1("coda_expr_tok_stack1.txt");
  TempFile f2("coda_expr_tok_stack2.txt");
  TempFile f3("coda_expr_tok_stack3.txt");
  TempFile f4("coda_expr_tok_stack4.txt");
  ASSERT_TRUE(f1.WriteText("11 12"));
  ASSERT_TRUE(f2.WriteText("21 22"));
  ASSERT_TRUE(f3.WriteText("31 32"));
  ASSERT_TRUE(f4.WriteText("41"));

  ScopedFile fp1(std::fopen(f1.c_str(), "rb"));
  ScopedFile fp2(std::fopen(f2.c_str(), "rb"));
  ScopedFile fp3(std::fopen(f3.c_str(), "rb"));
  ScopedFile fp4(std::fopen(f4.c_str(), "rb"));
  ASSERT_NE(nullptr, fp1);
  ASSERT_NE(nullptr, fp2);
  ASSERT_NE(nullptr, fp3);
  ASSERT_NE(nullptr, fp4);

  ScopedExprScanner scanner;
  ASSERT_EQ(0, scanner.Init());

  // Null / empty-stack calls are safe no-ops.
  coda_expression_pop_buffer_state(scanner.get());
  coda_expression_push_buffer_state(nullptr, scanner.get());
  coda_expression__flush_buffer(nullptr, scanner.get());
  coda_expression__delete_buffer(nullptr, scanner.get());

  // Push 3 buffers to trigger yyensure_buffer_stack() initial alloc and growth.
  YY_BUFFER_STATE b1 =
      coda_expression__create_buffer(fp1.get(), 64, scanner.get());
  ASSERT_NE(nullptr, b1);
  coda_expression_push_buffer_state(b1, scanner.get());

  YY_BUFFER_STATE b2 =
      coda_expression__create_buffer(fp2.get(), 64, scanner.get());
  ASSERT_NE(nullptr, b2);
  coda_expression_push_buffer_state(b2, scanner.get());

  YY_BUFFER_STATE b3 =
      coda_expression__create_buffer(fp3.get(), 64, scanner.get());
  ASSERT_NE(nullptr, b3);
  coda_expression_push_buffer_state(b3, scanner.get());

  YY_BUFFER_STATE b4 =
      coda_expression__create_buffer(fp4.get(), 64, scanner.get());
  ASSERT_NE(nullptr, b4);
  absl::Cleanup delete_b4 = [&] {
    coda_expression__delete_buffer(b4, scanner.get());
  };

  // Switching to the already-current buffer is a no-op.
  coda_expression__switch_to_buffer(b3, scanner.get());

  std::string actual_str;
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("31", actual_str);

  // Flushing the active buffer discards buffered " 32" and forces EOF next.
  coda_expression__flush_buffer(b3, scanner.get());
  EXPECT_EQ(0, NextToken(scanner.get()));

  // Flush a non-current buffer, switch to it, read its token, then switch back
  // and delete the non-current buffer.
  coda_expression__flush_buffer(b4, scanner.get());
  coda_expression__switch_to_buffer(b4, scanner.get());
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("41", actual_str);
  coda_expression__switch_to_buffer(b3, scanner.get());
  std::move(delete_b4).Invoke();

  // Pop back through b2 and b1, confirming buffered tokens resume properly.
  coda_expression_pop_buffer_state(scanner.get());
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("21", actual_str);
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("22", actual_str);

  coda_expression_pop_buffer_state(scanner.get());
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("11", actual_str);
  EXPECT_EQ(INT_VALUE, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("12", actual_str);

  // Pop final buffer so stack becomes empty.
  coda_expression_pop_buffer_state(scanner.get());
  coda_expression_pop_buffer_state(scanner.get());
}

TEST(CodaExprTokenizerTest, ScannerInitErrorsAccessorsAndAllocator) {
  errno = 0;
  EXPECT_EQ(1, coda_expression_lex_init(nullptr));
  EXPECT_EQ(EINVAL, errno);

  int initial_extra = 11;
  int updated_extra = 22;
  errno = 0;
  EXPECT_EQ(1, coda_expression_lex_init_extra(&initial_extra, nullptr));
  EXPECT_EQ(EINVAL, errno);

  TempFile dummy_file("coda_expr_tok_io.txt");
  ASSERT_TRUE(dummy_file.WriteText("x"));
  ScopedFile fp(std::fopen(dummy_file.c_str(), "r+b"));
  ASSERT_NE(nullptr, fp);

  ScopedExprScanner scanner;
  ASSERT_EQ(0, scanner.InitExtra(&initial_extra));
  EXPECT_EQ(&initial_extra, coda_expression_get_extra(scanner.get()));

  coda_expression_set_extra(&updated_extra, scanner.get());
  EXPECT_EQ(&updated_extra, coda_expression_get_extra(scanner.get()));

  // Without an active buffer, lineno and column getters return 0.
  EXPECT_EQ(0, coda_expression_get_lineno(scanner.get()));
  EXPECT_EQ(0, coda_expression_get_column(scanner.get()));

  YY_BUFFER_STATE buf = coda_expression__scan_string("alpha", scanner.get());
  ASSERT_NE(nullptr, buf);

  coda_expression_set_lineno(42, scanner.get());
  EXPECT_EQ(42, coda_expression_get_lineno(scanner.get()));

  coda_expression_set_column(7, scanner.get());
  EXPECT_EQ(7, coda_expression_get_column(scanner.get()));

  coda_expression_set_debug(1, scanner.get());
  EXPECT_EQ(1, coda_expression_get_debug(scanner.get()));
  coda_expression_set_debug(0, scanner.get());
  EXPECT_EQ(0, coda_expression_get_debug(scanner.get()));

  coda_expression_set_in(fp.get(), scanner.get());
  EXPECT_EQ(fp.get(), coda_expression_get_in(scanner.get()));
  coda_expression_set_in(nullptr, scanner.get());
  EXPECT_EQ(nullptr, coda_expression_get_in(scanner.get()));

  coda_expression_set_out(fp.get(), scanner.get());
  EXPECT_EQ(fp.get(), coda_expression_get_out(scanner.get()));
  coda_expression_set_out(nullptr, scanner.get());
  EXPECT_EQ(nullptr, coda_expression_get_out(scanner.get()));

  YYSTYPE custom_lval{};
  coda_expression_set_lval(&custom_lval, scanner.get());
  EXPECT_EQ(&custom_lval, coda_expression_get_lval(scanner.get()));

  std::string actual_str;
  EXPECT_EQ(NAME, NextToken(scanner.get(), &actual_str));
  EXPECT_EQ("alpha", actual_str);

  void* raw = coda_expression_alloc(16, scanner.get());
  ASSERT_NE(nullptr, raw);
  std::memset(raw, 0x5A, 16);
  void* grown = coda_expression_realloc(raw, 64, scanner.get());
  ASSERT_NE(nullptr, grown);
  const auto* bytes = static_cast<const uint8_t*>(grown);
  for (size_t idx = 0; idx < 16; ++idx) {
    EXPECT_EQ(0x5AU, bytes[idx]);
  }
  coda_expression_free(grown, scanner.get());
}

// TODO(schwehr): 85.6% reachable ceiling in libcoda/coda-expr-tokenizer.c
// (857/1001 via standalone llvm-cov); missed lines are 77 dead YY_BREAK/'}'
// lines after return, 45 dead Flex skeleton lines, 13 OOM lines, and 9 exit(2)
// yy_fatal_error lines.

}  // namespace
}  // namespace stcorp_coda
