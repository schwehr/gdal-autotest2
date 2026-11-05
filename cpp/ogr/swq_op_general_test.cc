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

#include <climits>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

#define GDAL_COMPILATION
#include "third_party/gdal/ogr/ogr_swq.h"

namespace autotest2 {
namespace {

// Helper to evaluate general operations safely. Sub-expressions are owned and
// freed by the parent opNode upon return.
swq_expr_node* EvalOp(swq_op op, swq_field_type target_type,
                      std::vector<swq_expr_node*> args,
                      const swq_evaluation_context& sContext = {}) {
  swq_expr_node node(op);
  node.field_type = target_type;
  for (swq_expr_node* arg : args) {
    node.PushSubExpression(arg);
  }
  return SWQGeneralEvaluator(&node, node.papoSubExpr, sContext);
}

// Helper to evaluate cast operations safely. Sub-expressions are owned and
// freed by the parent castNode upon return.
swq_expr_node* EvalCast(swq_field_type target_type,
                        std::vector<swq_expr_node*> args,
                        const swq_evaluation_context& sContext = {}) {
  swq_expr_node node(SWQ_CAST);
  node.field_type = target_type;
  for (swq_expr_node* arg : args) {
    node.PushSubExpression(arg);
  }
  return SWQCastEvaluator(&node, node.papoSubExpr, sContext);
}

// =========================================================================
// swq_test_like tests
// =========================================================================

TEST(SwqOpGeneralTest, SwqTestLikeNullInputs) {
  EXPECT_EQ(0, swq_test_like(nullptr, "pattern", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("input", nullptr, '\\', false, false));
  EXPECT_EQ(0, swq_test_like(nullptr, nullptr, '\\', false, false));
}

TEST(SwqOpGeneralTest, SwqTestLikeBasicPatterns) {
  // Empty inputs
  EXPECT_EQ(1, swq_test_like("", "", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("", "%", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("", "a", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("abc", "", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("abc", "ab", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("abc", "abc", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("abc", "abcd", '\\', false, false));

  // Trailing escape character
  EXPECT_EQ(0, swq_test_like("a", "\\", '\\', false, false));
  // Escaped match / mismatch
  EXPECT_EQ(1, swq_test_like("%", "\\%", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("a", "\\%", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("_", "\\_", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("a", "\\_", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("a\\b", "a\\\\b", '\\', false, false));

  // Single wildcard '_'
  EXPECT_EQ(1, swq_test_like("abc", "a_c", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("a", "_", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("ab", "_", '\\', false, false));

  // Multiple wildcard '%'
  EXPECT_EQ(1, swq_test_like("abcdef", "ab%", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("abcdef", "%ef", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("abcdef", "%cd%", '\\', false, false));
  EXPECT_EQ(1, swq_test_like("abcdef", "a%c%f", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("abcdef", "a%xyz%f", '\\', false, false));
}

TEST(SwqOpGeneralTest, SwqTestLikeCaseSensitivityAndUTF8) {
  // Case sensitive (insensitive = false)
  EXPECT_EQ(1, swq_test_like("Hello", "Hello", '\\', false, false));
  EXPECT_EQ(0, swq_test_like("Hello", "hello", '\\', false, false));

  // Case insensitive ASCII (insensitive = true, bUTF8Strings = false)
  EXPECT_EQ(1, swq_test_like("Hello", "hello", '\\', true, false));
  EXPECT_EQ(1, swq_test_like("WORLD", "%wor%", '\\', true, false));
  EXPECT_EQ(0, swq_test_like("WORLD", "wor", '\\', true, false));

  // UTF-8 single char wildcard '_'
  // Non-UTF8 mode considers each byte of multi-byte as a char
  EXPECT_EQ(1, swq_test_like("\xC3\xA9x", "__x", '\\', false, false));
  // UTF8 mode treats multi-byte sequence as 1 char
  EXPECT_EQ(1, swq_test_like("\xC3\xA9x", "_x", '\\', false, true));

  // UTF-8 case-insensitive comparison
  // \xC3\xA9 (é) vs \xC3\x89 (É)
  EXPECT_EQ(1, swq_test_like("\xC3\xA9", "\xC3\x89", '\\', true, true));
  EXPECT_EQ(0, swq_test_like("\xC3\xA9", "\xC3\x89", '\\', false, true));
  EXPECT_EQ(0, swq_test_like("\xC3\xA9", "\xC3\xA0", '\\', true, true));

  // 3-byte UTF-8 codepoint (Euro sign: \xE2\x82\xAC)
  EXPECT_EQ(1, swq_test_like("\xE2\x82\xAC", "\xE2\x82\xAC", '\\', true, true));

  // Incomplete / truncated UTF-8 sequences to test IsStringLongEnough boundary
  const char truncatedPattern[] = {'\xC3', '\0'};
  EXPECT_EQ(0, swq_test_like("a", truncatedPattern, '\\', true, true));
  const char truncatedPattern3[] = {'\xE2', '\x82', '\0'};
  EXPECT_EQ(0, swq_test_like("a", truncatedPattern3, '\\', true, true));

  const char truncatedInput[] = {'\xC3', '\0'};
  EXPECT_EQ(0, swq_test_like(truncatedInput, "\xC3\xA9", '\\', true, true));
  const char truncatedInput3[] = {'\xE2', '\x82', '\0'};
  EXPECT_EQ(0,
            swq_test_like(truncatedInput3, "\xE2\x82\xAC", '\\', true, true));
}

// =========================================================================
// OGRHStoreGetValue tests
// =========================================================================

TEST(SwqOpGeneralTest, HStoreGetValueBasic) {
  const char* hstore = "k1=>v1,k2=>v2";
  char* val = OGRHStoreGetValue(hstore, "k1");
  ASSERT_NE(nullptr, val);
  EXPECT_STREQ("v1", val);
  CPLFree(val);

  val = OGRHStoreGetValue(hstore, "k2");
  ASSERT_NE(nullptr, val);
  EXPECT_STREQ("v2", val);
  CPLFree(val);

  val = OGRHStoreGetValue(hstore, "k3");
  EXPECT_EQ(nullptr, val);
}

TEST(SwqOpGeneralTest, HStoreGetValueSpacesAndQuotes) {
  const char* hstore =
      "  \"key with space\"  =>  \"value with space\"  ,  unquoted_k  =>  "
      "unquoted_v  ";
  char* val = OGRHStoreGetValue(hstore, "key with space");
  ASSERT_NE(nullptr, val);
  EXPECT_STREQ("value with space", val);
  CPLFree(val);

  val = OGRHStoreGetValue(hstore, "unquoted_k");
  ASSERT_NE(nullptr, val);
  EXPECT_STREQ("unquoted_v", val);
  CPLFree(val);

  // Escaped quotes and backslashes in quoted strings
  const char* escapedHStore = "\"k\\\"1\\\\\" => \"v\\\"1\\\\\"";
  val = OGRHStoreGetValue(escapedHStore, "k\"1\\");
  ASSERT_NE(nullptr, val);
  EXPECT_STREQ("v\"1\\", val);
  CPLFree(val);
}

TEST(SwqOpGeneralTest, HStoreGetValueMalformed) {
  // Empty string
  EXPECT_EQ(nullptr, OGRHStoreGetValue("", "k"));

  // Incomplete quote
  EXPECT_EQ(nullptr, OGRHStoreGetValue("\"k", "k"));
  EXPECT_EQ(nullptr, OGRHStoreGetValue("\"k\" => \"v", "k"));

  // Trailing backslash in quote
  EXPECT_EQ(nullptr, OGRHStoreGetValue("\"k\\", "k"));

  // Malformed separator after key
  EXPECT_EQ(nullptr, OGRHStoreGetValue("\"k\" =x \"v\"", "k"));
  EXPECT_EQ(nullptr, OGRHStoreGetValue("\"k\" extra => \"v\"", "k"));

  // Malformed separator after value
  EXPECT_EQ(nullptr, OGRHStoreGetValue("\"k\" => \"v\" extra, k2 => v2", "k"));

  // Unquoted key without =>
  EXPECT_EQ(nullptr, OGRHStoreGetValue("key_only", "key_only"));
}

// =========================================================================
// SWQGeneralEvaluator: Float operations
// =========================================================================

TEST(SwqOpGeneralTest, EvaluatorFloatBasics) {
  // Addition with float and integer promotion
  {
    swq_expr_node* ret = EvalOp(
        SWQ_ADD, SWQ_FLOAT, {new swq_expr_node(10.5), new swq_expr_node(5)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(15.5, ret->float_value);
    EXPECT_EQ(0, ret->is_null);
    delete ret;
  }

  // Subtraction, multiplication, division, modulus
  {
    swq_expr_node* ret =
        EvalOp(SWQ_SUBTRACT, SWQ_FLOAT,
               {new swq_expr_node(10.0), new swq_expr_node(2.5)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(7.5, ret->float_value);
    delete ret;

    ret = EvalOp(SWQ_MULTIPLY, SWQ_FLOAT,
                 {new swq_expr_node(10.0), new swq_expr_node(2.5)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(25.0, ret->float_value);
    delete ret;

    ret = EvalOp(SWQ_DIVIDE, SWQ_FLOAT,
                 {new swq_expr_node(10.0), new swq_expr_node(2.5)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(4.0, ret->float_value);
    delete ret;

    ret = EvalOp(SWQ_MODULUS, SWQ_FLOAT,
                 {new swq_expr_node(7.5), new swq_expr_node(2.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(1.5, ret->float_value);
    delete ret;

    // Division and modulus by zero -> INT_MAX
    ret = EvalOp(SWQ_DIVIDE, SWQ_FLOAT,
                 {new swq_expr_node(10.0), new swq_expr_node(0.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(static_cast<double>(INT_MAX), ret->float_value);
    delete ret;

    ret = EvalOp(SWQ_MODULUS, SWQ_FLOAT,
                 {new swq_expr_node(10.0), new swq_expr_node(0.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(static_cast<double>(INT_MAX), ret->float_value);
    delete ret;
  }

  // Float comparisons: EQ, NE, GT, LT, GE, LE, BETWEEN, ISNULL
  {
    swq_expr_node* ret = EvalOp(
        SWQ_EQ, SWQ_BOOLEAN, {new swq_expr_node(5.0), new swq_expr_node(5.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->int_value);
    delete ret;

    ret = EvalOp(SWQ_NE, SWQ_BOOLEAN,
                 {new swq_expr_node(5.0), new swq_expr_node(5.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->int_value);
    delete ret;

    ret = EvalOp(SWQ_GT, SWQ_BOOLEAN,
                 {new swq_expr_node(5.0), new swq_expr_node(10.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->int_value);
    delete ret;

    ret = EvalOp(SWQ_LT, SWQ_BOOLEAN,
                 {new swq_expr_node(5.0), new swq_expr_node(10.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->int_value);
    delete ret;

    ret = EvalOp(SWQ_GE, SWQ_BOOLEAN,
                 {new swq_expr_node(5.0), new swq_expr_node(5.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->int_value);
    delete ret;

    ret = EvalOp(SWQ_LE, SWQ_BOOLEAN,
                 {new swq_expr_node(5.0), new swq_expr_node(5.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->int_value);
    delete ret;

    // BETWEEN
    ret = EvalOp(SWQ_BETWEEN, SWQ_BOOLEAN,
                 {new swq_expr_node(5.0), new swq_expr_node(2.0),
                  new swq_expr_node(10.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->int_value);
    delete ret;

    // ISNULL
    ret = EvalOp(SWQ_ISNULL, SWQ_BOOLEAN, {new swq_expr_node(5.0)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->int_value);
    delete ret;
  }
}

TEST(SwqOpGeneralTest, EvaluatorFloatNullPropagationAndIn) {
  // Null propagation for float operation returning BOOLEAN, FLOAT, and INTEGER
  auto makeNull = []() {
    auto* n = new swq_expr_node(0.0);
    n->is_null = 1;
    return n;
  };

  swq_expr_node* ret =
      EvalOp(SWQ_EQ, SWQ_BOOLEAN, {makeNull(), new swq_expr_node(3.5)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_ADD, SWQ_FLOAT, {makeNull(), new swq_expr_node(3.5)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_ADD, SWQ_INTEGER, {makeNull(), new swq_expr_node(3.5)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // SWQ_IN for float
  // Case 1: arg0 is null
  ret = EvalOp(
      SWQ_IN, SWQ_BOOLEAN,
      {makeNull(), new swq_expr_node(1.0), new swq_expr_node(2.0), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Case 2: arg0 matches element
  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node(2.0), new swq_expr_node(1.0),
                new swq_expr_node(2.0), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  EXPECT_EQ(0, ret->is_null);
  delete ret;

  // Case 3: arg0 not matched, but null is present in list -> is_null = 1
  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node(99.0), new swq_expr_node(1.0),
                new swq_expr_node(2.0), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Case 4: arg0 not matched, no null in list
  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node(99.0), new swq_expr_node(1.0),
                new swq_expr_node(2.0)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  EXPECT_EQ(0, ret->is_null);
  delete ret;
}

// =========================================================================
// SWQGeneralEvaluator: Integer and Boolean operations
// =========================================================================

TEST(SwqOpGeneralTest, EvaluatorIntegerAndBoolOperations) {
  auto makeNull = []() {
    auto* n = new swq_expr_node(0);
    n->is_null = 1;
    return n;
  };

  // Logical operations: AND, OR, NOT
  swq_expr_node* ret = EvalOp(SWQ_AND, SWQ_BOOLEAN,
                              {new swq_expr_node(1), new swq_expr_node(0)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  EXPECT_EQ(0, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_AND, SWQ_BOOLEAN, {makeNull(), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_OR, SWQ_BOOLEAN, {new swq_expr_node(1), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_NOT, SWQ_BOOLEAN, {new swq_expr_node(1)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_NOT, SWQ_BOOLEAN, {makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  // Comparisons: EQ, NE, GT, LT, GE, LE, BETWEEN, ISNULL
  ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(10)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_NE, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(20)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GT, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(20)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LT, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(20)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GE, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(10)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LE, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(10)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(
      SWQ_BETWEEN, SWQ_BOOLEAN,
      {new swq_expr_node(10), new swq_expr_node(5), new swq_expr_node(20)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_ISNULL, SWQ_BOOLEAN, {makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // SWQ_IN for integer
  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {makeNull(), new swq_expr_node(5), new swq_expr_node(10),
                new swq_expr_node(20)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(5),
                new swq_expr_node(10), new swq_expr_node(20)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node(10), new swq_expr_node(5),
                new swq_expr_node(20), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Null propagation for normal integer operation
  ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN, {makeNull(), new swq_expr_node(10)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;
}

TEST(SwqOpGeneralTest, EvaluatorIntegerArithmeticAndOverflow) {
  WithQuietHandler quietHandler;

  // Normal addition
  swq_expr_node* ret = EvalOp(SWQ_ADD, SWQ_INTEGER64,
                              {new swq_expr_node(10), new swq_expr_node(25)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(35, ret->int_value);
  delete ret;

  // Addition overflow
  ret = EvalOp(SWQ_ADD, SWQ_INTEGER64,
               {new swq_expr_node(std::numeric_limits<GIntBig>::max()),
                new swq_expr_node(1)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Subtraction overflow
  ret = EvalOp(SWQ_SUBTRACT, SWQ_INTEGER64,
               {new swq_expr_node(std::numeric_limits<GIntBig>::min()),
                new swq_expr_node(1)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Multiplication overflow
  ret = EvalOp(SWQ_MULTIPLY, SWQ_INTEGER64,
               {new swq_expr_node(std::numeric_limits<GIntBig>::max()),
                new swq_expr_node(2)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Division normal, division by zero, and division overflow
  ret = EvalOp(SWQ_DIVIDE, SWQ_INTEGER64,
               {new swq_expr_node(10), new swq_expr_node(0)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(INT_MAX, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_DIVIDE, SWQ_INTEGER64,
               {new swq_expr_node(std::numeric_limits<GIntBig>::min()),
                new swq_expr_node(-1)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Modulus normal and mod by zero
  ret = EvalOp(SWQ_MODULUS, SWQ_INTEGER64,
               {new swq_expr_node(10), new swq_expr_node(0)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(INT_MAX, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_MODULUS, SWQ_INTEGER64,
               {new swq_expr_node(17), new swq_expr_node(5)});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(2, ret->int_value);
  delete ret;
}

// =========================================================================
// SWQGeneralEvaluator: Timestamp operations
// =========================================================================

TEST(SwqOpGeneralTest, EvaluatorTimestampOperations) {
  WithQuietHandler quietHandler;

  auto makeTs = [](const char* str) {
    auto* n = new swq_expr_node(str);
    n->field_type = SWQ_TIMESTAMP;
    return n;
  };

  auto makeNullTs = []() {
    auto* n = new swq_expr_node("2023/01/01");
    n->field_type = SWQ_TIMESTAMP;
    n->is_null = 1;
    return n;
  };

  // Null operand
  swq_expr_node* ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN,
                              {makeNullTs(), makeTs("2023/01/02 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Invalid date strings
  EXPECT_EQ(nullptr,
            EvalOp(SWQ_EQ, SWQ_BOOLEAN,
                   {makeTs("invalid_date"), makeTs("2023/01/02 12:00:00")}));

  EXPECT_EQ(nullptr,
            EvalOp(SWQ_EQ, SWQ_BOOLEAN,
                   {makeTs("2023/01/02 12:00:00"), makeTs("invalid_date")}));

  // Valid date comparisons: GT, GE, LT, LE, EQ, BETWEEN
  ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN,
               {makeTs("2023/05/01 12:00:00"), makeTs("2023/05/02 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LT, SWQ_BOOLEAN,
               {makeTs("2023/05/01 12:00:00"), makeTs("2023/05/02 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LE, SWQ_BOOLEAN,
               {makeTs("2023/05/01 12:00:00"), makeTs("2023/05/02 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GT, SWQ_BOOLEAN,
               {makeTs("2023/05/01 12:00:00"), makeTs("2023/05/02 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GE, SWQ_BOOLEAN,
               {makeTs("2023/05/01 12:00:00"), makeTs("2023/05/01 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // BETWEEN
  ret = EvalOp(SWQ_BETWEEN, SWQ_BOOLEAN,
               {makeTs("2023/05/02 12:00:00"), makeTs("2023/05/01 12:00:00"),
                makeTs("2023/05/03 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // BETWEEN with invalid 3rd arg
  EXPECT_EQ(nullptr,
            EvalOp(SWQ_BETWEEN, SWQ_BOOLEAN,
                   {makeTs("2023/05/02 12:00:00"),
                    makeTs("2023/05/01 12:00:00"), makeTs("invalid_date")}));

  // SWQ_IN with timestamps
  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {makeNullTs(), makeTs("2023/05/01 12:00:00"),
                makeTs("2023/05/02 12:00:00"), makeTs("2023/05/03 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {makeTs("2023/05/02 12:00:00"), makeTs("2023/05/01 12:00:00"),
                makeTs("2023/05/02 12:00:00"), makeNullTs()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {makeTs("2023/05/01 12:00:00"), makeTs("2023/05/02 12:00:00"),
                makeTs("2023/05/03 12:00:00"), makeNullTs()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  EXPECT_EQ(nullptr,
            EvalOp(SWQ_IN, SWQ_BOOLEAN,
                   {makeTs("2023/05/01 12:00:00"), makeTs("invalid_date"),
                    makeTs("2023/05/03 12:00:00")}));
}

// =========================================================================
// SWQGeneralEvaluator: String operations
// =========================================================================

TEST(SwqOpGeneralTest, EvaluatorStringOperations) {
  auto makeNull = []() {
    auto* n = new swq_expr_node("null");
    n->is_null = 1;
    return n;
  };

  auto makeNoStr = []() {
    auto* n = new swq_expr_node();
    n->field_type = SWQ_DATE;
    return n;
  };

  auto makeDateStr = [](const char* str) {
    auto* n = new swq_expr_node(str);
    n->field_type = SWQ_DATE;
    return n;
  };

  // Null string propagation
  swq_expr_node* ret =
      EvalOp(SWQ_EQ, SWQ_BOOLEAN, {makeNull(), new swq_expr_node("hello")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret =
      EvalOp(SWQ_CONCAT, SWQ_STRING, {makeNull(), new swq_expr_node("hello")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Timestamp strings with +00 timezone ending
  ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN,
               {new swq_expr_node("2023-01-01 12:00:00+00"),
                new swq_expr_node("2023-01-01 12:00:00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN,
               {new swq_expr_node("2023-01-01 12:00:00"),
                new swq_expr_node("2023-01-01 12:00:00+00")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // Comparisons: NE, GT, LT, GE, LE, BETWEEN
  ret = EvalOp(SWQ_NE, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("banana")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GT, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("banana")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LT, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("banana")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GE, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("apple")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LE, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("apple")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_BETWEEN, SWQ_BOOLEAN,
               {new swq_expr_node("banana"), new swq_expr_node("apple"),
                new swq_expr_node("cherry")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // Null string_value in operand 1 with SWQ_DATE
  ret = EvalOp(SWQ_EQ, SWQ_BOOLEAN, {makeDateStr("2023-01-01"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_NE, SWQ_BOOLEAN, {makeDateStr("2023-01-01"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GT, SWQ_BOOLEAN, {makeDateStr("2023-01-01"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LT, SWQ_BOOLEAN, {makeDateStr("2023-01-01"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_GE, SWQ_BOOLEAN, {makeDateStr("2023-01-01"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_LE, SWQ_BOOLEAN, {makeDateStr("2023-01-01"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  ret = EvalOp(
      SWQ_BETWEEN, SWQ_BOOLEAN,
      {makeDateStr("2023-01-01"), makeNoStr(), makeDateStr("2023-01-02")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  // SWQ_IN
  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {makeNull(), new swq_expr_node("apple"),
                new swq_expr_node("banana"), new swq_expr_node("cherry")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("banana"),
                new swq_expr_node("apple"), new swq_expr_node("cherry")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  ret = EvalOp(SWQ_IN, SWQ_BOOLEAN,
               {new swq_expr_node("apple"), new swq_expr_node("banana"),
                new swq_expr_node("cherry"), makeNull()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // SWQ_LIKE and SWQ_ILIKE
  ret = EvalOp(SWQ_LIKE, SWQ_BOOLEAN,
               {new swq_expr_node("banana"), new swq_expr_node("%an%")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // With escape
  ret = EvalOp(SWQ_LIKE, SWQ_BOOLEAN,
               {new swq_expr_node("100%_apple"), new swq_expr_node("%\\%\\_%"),
                new swq_expr_node("\\")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // Null pattern in LIKE
  ret =
      EvalOp(SWQ_LIKE, SWQ_BOOLEAN, {new swq_expr_node("banana"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  // SWQ_ILIKE
  ret = EvalOp(SWQ_ILIKE, SWQ_BOOLEAN,
               {new swq_expr_node("banana"), new swq_expr_node("%AN%")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->int_value);
  delete ret;

  // Null pattern in ILIKE
  ret = EvalOp(SWQ_ILIKE, SWQ_BOOLEAN,
               {new swq_expr_node("banana"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;

  // SUBSTR
  // 2 arguments (from offset to end)
  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(3)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("cdefghij", ret->string_value);
  delete ret;

  // 3 arguments with integer offset and length
  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(3),
                new swq_expr_node(4)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("cdef", ret->string_value);
  delete ret;

  // 3 arguments with integer offset and float length
  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(3),
                new swq_expr_node(4.0)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("cdef", ret->string_value);
  delete ret;

  // Negative offset within string length
  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(-3)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("hij", ret->string_value);
  delete ret;

  // Negative offset exceeding string length
  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(-50)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("abcdefghij", ret->string_value);
  delete ret;

  // Negative length or offset past end
  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(50)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("", ret->string_value);
  delete ret;

  ret = EvalOp(SWQ_SUBSTR, SWQ_STRING,
               {new swq_expr_node("abcdefghij"), new swq_expr_node(3),
                new swq_expr_node(-1)});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("", ret->string_value);
  delete ret;

  // HSTORE_GET_VALUE
  ret = EvalOp(
      SWQ_HSTORE_GET_VALUE, SWQ_STRING,
      {new swq_expr_node("foo=>bar,baz=>qux"), new swq_expr_node("foo")});
  ASSERT_NE(nullptr, ret);
  EXPECT_STREQ("bar", ret->string_value);
  EXPECT_EQ(0, ret->is_null);
  delete ret;

  // Key not found
  ret = EvalOp(
      SWQ_HSTORE_GET_VALUE, SWQ_STRING,
      {new swq_expr_node("foo=>bar,baz=>qux"), new swq_expr_node("missing")});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(1, ret->is_null);
  delete ret;

  // Null key string
  ret = EvalOp(SWQ_HSTORE_GET_VALUE, SWQ_STRING,
               {new swq_expr_node("foo=>bar,baz=>qux"), makeNoStr()});
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(0, ret->int_value);
  delete ret;
}

// =========================================================================
// SWQGeneralChecker tests
// =========================================================================

TEST(SwqOpGeneralTest, GeneralCheckerTypePromotionsAndConversions) {
  WithQuietHandler quietHandler;

  // Auto promotion of Integer to Float
  {
    swq_expr_node node(SWQ_EQ);
    node.PushSubExpression(new swq_expr_node(10));    // int constant
    node.PushSubExpression(new swq_expr_node(20.5));  // float constant
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));
    EXPECT_EQ(SWQ_FLOAT, node.papoSubExpr[0]->field_type);
  }

  // Auto promotion of Integer to Integer64
  {
    swq_expr_node node(SWQ_EQ);
    node.PushSubExpression(new swq_expr_node(10));  // int constant
    swq_expr_node* i64 = new swq_expr_node(static_cast<GIntBig>(50));
    node.PushSubExpression(i64);
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));
    EXPECT_EQ(SWQ_INTEGER64, node.papoSubExpr[0]->field_type);
  }

  // Auto promotion of String to Timestamp
  {
    swq_expr_node node(SWQ_EQ);
    node.PushSubExpression(new swq_expr_node("2023-01-01"));
    swq_expr_node* ts = new swq_expr_node("2023-01-02");
    ts->field_type = SWQ_TIMESTAMP;
    node.PushSubExpression(ts);
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));
    EXPECT_EQ(SWQ_TIMESTAMP, node.papoSubExpr[0]->field_type);
  }

  // Auto conversion of String constant to Numeric
  {
    swq_expr_node node(SWQ_EQ);
    node.PushSubExpression(new swq_expr_node("123.45"));
    node.PushSubExpression(new swq_expr_node(50.0));
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));
    EXPECT_EQ(SWQ_FLOAT, node.papoSubExpr[0]->field_type);
    EXPECT_DOUBLE_EQ(123.45, node.papoSubExpr[0]->float_value);
  }

  // Invalid numeric string conversion warning
  {
    swq_expr_node node(SWQ_EQ);
    node.PushSubExpression(new swq_expr_node("not_a_number"));
    node.PushSubExpression(new swq_expr_node(50.0));
    // Conversion fails, type remains string, then type mismatch fails check
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }
}

TEST(SwqOpGeneralTest, GeneralCheckerGeometryRejection) {
  WithQuietHandler quietHandler;

  // Operations that reject geometry
  std::vector<swq_op> ops = {
      SWQ_AND,      SWQ_EQ,     SWQ_LIKE,   SWQ_ADD,
      SWQ_SUBTRACT, SWQ_CONCAT, SWQ_SUBSTR, SWQ_HSTORE_GET_VALUE};

  for (swq_op op : ops) {
    swq_expr_node node(op);
    OGRPoint pt(1.0, 2.0);
    node.PushSubExpression(new swq_expr_node(&pt));
    node.PushSubExpression(new swq_expr_node(&pt));
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }
}

TEST(SwqOpGeneralTest, GeneralCheckerOperationsReturnTypes) {
  // Logical operations
  {
    swq_expr_node node(SWQ_AND);
    node.PushSubExpression(new swq_expr_node(1));
    node.PushSubExpression(new swq_expr_node(0));
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));
  }

  // ISNULL
  {
    swq_expr_node node(SWQ_ISNULL);
    node.PushSubExpression(new swq_expr_node(1));
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));
  }

  // LIKE and ILIKE
  {
    swq_expr_node node(SWQ_LIKE);
    node.PushSubExpression(new swq_expr_node("hello"));
    node.PushSubExpression(new swq_expr_node("h%"));
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, FALSE));

    swq_expr_node nodeI(SWQ_ILIKE);
    nodeI.PushSubExpression(new swq_expr_node("hello"));
    nodeI.PushSubExpression(new swq_expr_node("H%"));
    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&nodeI, FALSE));
  }

  // ADD for String, Float, Integer64, Integer
  {
    swq_expr_node nodeStr(SWQ_ADD);
    nodeStr.PushSubExpression(new swq_expr_node("hello"));
    nodeStr.PushSubExpression(new swq_expr_node("world"));
    EXPECT_EQ(SWQ_STRING, SWQGeneralChecker(&nodeStr, FALSE));

    swq_expr_node nodeFloat(SWQ_ADD);
    nodeFloat.PushSubExpression(new swq_expr_node(1.5));
    nodeFloat.PushSubExpression(new swq_expr_node(2.5));
    EXPECT_EQ(SWQ_FLOAT, SWQGeneralChecker(&nodeFloat, FALSE));

    swq_expr_node nodeI64(SWQ_ADD);
    nodeI64.PushSubExpression(
        new swq_expr_node(static_cast<GIntBig>(100000000000LL)));
    nodeI64.PushSubExpression(
        new swq_expr_node(static_cast<GIntBig>(200000000000LL)));
    EXPECT_EQ(SWQ_INTEGER64, SWQGeneralChecker(&nodeI64, FALSE));

    swq_expr_node nodeInt(SWQ_ADD);
    nodeInt.PushSubExpression(new swq_expr_node(10));
    nodeInt.PushSubExpression(new swq_expr_node(20));
    EXPECT_EQ(SWQ_INTEGER, SWQGeneralChecker(&nodeInt, FALSE));
  }

  // SUBTRACT, MULTIPLY, DIVIDE, MODULUS return types
  {
    swq_expr_node nodeSub(SWQ_SUBTRACT);
    nodeSub.PushSubExpression(new swq_expr_node(10));
    nodeSub.PushSubExpression(new swq_expr_node(5));
    EXPECT_EQ(SWQ_INTEGER, SWQGeneralChecker(&nodeSub, FALSE));

    swq_expr_node nodeMul(SWQ_MULTIPLY);
    nodeMul.PushSubExpression(new swq_expr_node(1.5));
    nodeMul.PushSubExpression(new swq_expr_node(2.0));
    EXPECT_EQ(SWQ_FLOAT, SWQGeneralChecker(&nodeMul, FALSE));

    swq_expr_node nodeDiv(SWQ_DIVIDE);
    nodeDiv.PushSubExpression(
        new swq_expr_node(static_cast<GIntBig>(100000000000LL)));
    nodeDiv.PushSubExpression(new swq_expr_node(static_cast<GIntBig>(2LL)));
    EXPECT_EQ(SWQ_INTEGER64, SWQGeneralChecker(&nodeDiv, FALSE));
  }

  // CONCAT
  {
    swq_expr_node nodeConcat(SWQ_CONCAT);
    nodeConcat.PushSubExpression(new swq_expr_node("part1"));
    nodeConcat.PushSubExpression(new swq_expr_node("part2"));
    EXPECT_EQ(SWQ_STRING, SWQGeneralChecker(&nodeConcat, FALSE));
  }
}

TEST(SwqOpGeneralTest, GeneralCheckerSubstrAndHStoreErrors) {
  WithQuietHandler quietHandler;

  // SUBSTR wrong arg count (< 2 or > 3)
  {
    swq_expr_node node(SWQ_SUBSTR);
    node.PushSubExpression(new swq_expr_node("text"));
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }
  {
    swq_expr_node node(SWQ_SUBSTR);
    node.PushSubExpression(new swq_expr_node("text"));
    node.PushSubExpression(new swq_expr_node(1));
    node.PushSubExpression(new swq_expr_node(2));
    node.PushSubExpression(new swq_expr_node(3));
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }

  // SUBSTR wrong argument types
  {
    swq_expr_node node(SWQ_SUBSTR);
    node.PushSubExpression(new swq_expr_node(123));  // expected string
    node.PushSubExpression(new swq_expr_node(1));
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }

  // Valid SUBSTR
  {
    swq_expr_node node(SWQ_SUBSTR);
    node.PushSubExpression(new swq_expr_node("text"));
    node.PushSubExpression(new swq_expr_node(1));
    node.PushSubExpression(new swq_expr_node(2));
    EXPECT_EQ(SWQ_STRING, SWQGeneralChecker(&node, FALSE));
  }

  // HSTORE_GET_VALUE wrong arg count
  {
    swq_expr_node node(SWQ_HSTORE_GET_VALUE);
    node.PushSubExpression(new swq_expr_node("k=>v"));
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }

  // HSTORE_GET_VALUE wrong argument types
  {
    swq_expr_node node(SWQ_HSTORE_GET_VALUE);
    node.PushSubExpression(new swq_expr_node("k=>v"));
    node.PushSubExpression(new swq_expr_node(123));  // expected string
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }

  // Valid HSTORE_GET_VALUE
  {
    swq_expr_node node(SWQ_HSTORE_GET_VALUE);
    node.PushSubExpression(new swq_expr_node("k=>v"));
    node.PushSubExpression(new swq_expr_node("k"));
    EXPECT_EQ(SWQ_STRING, SWQGeneralChecker(&node, FALSE));
  }

  // Unsupported operation
  {
    swq_expr_node node(SWQ_AVG);
    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }
}

TEST(SwqOpGeneralTest, GeneralCheckerTypeMismatchAndAllowFieldMismatch) {
  WithQuietHandler quietHandler;

  // Type mismatch with bAllowMismatchTypeOnFieldComparison = FALSE
  {
    swq_expr_node node(SWQ_EQ);
    swq_expr_node* col1 = new swq_expr_node("col1");
    col1->eNodeType = SNT_COLUMN;
    col1->field_type = SWQ_FLOAT;

    swq_expr_node* col2 = new swq_expr_node("col2");
    col2->eNodeType = SNT_COLUMN;
    col2->field_type = SWQ_STRING;

    node.PushSubExpression(col1);
    node.PushSubExpression(col2);

    EXPECT_EQ(SWQ_ERROR, SWQGeneralChecker(&node, FALSE));
  }

  // Type mismatch allowed: col1 is float, col2 is string -> col2 cast to float
  {
    swq_expr_node node(SWQ_EQ);
    swq_expr_node* col1 = new swq_expr_node("col1");
    col1->eNodeType = SNT_COLUMN;
    col1->field_type = SWQ_FLOAT;

    swq_expr_node* col2 = new swq_expr_node("col2");
    col2->eNodeType = SNT_COLUMN;
    col2->field_type = SWQ_STRING;

    node.PushSubExpression(col1);
    node.PushSubExpression(col2);

    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, TRUE));
    // col2 should now be a SWQ_CAST operation node
    EXPECT_EQ(SNT_OPERATION, node.papoSubExpr[1]->eNodeType);
    EXPECT_EQ(SWQ_CAST, node.papoSubExpr[1]->nOperation);
  }

  // Type mismatch allowed: col1 is string, col2 is float -> col1 cast to float
  {
    swq_expr_node node(SWQ_EQ);
    swq_expr_node* col1 = new swq_expr_node("col1");
    col1->eNodeType = SNT_COLUMN;
    col1->field_type = SWQ_STRING;

    swq_expr_node* col2 = new swq_expr_node("col2");
    col2->eNodeType = SNT_COLUMN;
    col2->field_type = SWQ_FLOAT;

    node.PushSubExpression(col1);
    node.PushSubExpression(col2);

    EXPECT_EQ(SWQ_BOOLEAN, SWQGeneralChecker(&node, TRUE));
    // col1 should now be a SWQ_CAST operation node
    EXPECT_EQ(SNT_OPERATION, node.papoSubExpr[0]->eNodeType);
    EXPECT_EQ(SWQ_CAST, node.papoSubExpr[0]->nOperation);
  }
}

// =========================================================================
// SWQCastChecker and SWQCastEvaluator tests
// =========================================================================

TEST(SwqOpGeneralTest, CastCheckerValidAndInvalidTypes) {
  WithQuietHandler quietHandler;

  // Valid casts
  std::vector<std::pair<const char*, swq_field_type>> validTypes = {
      {"boolean", SWQ_BOOLEAN},  {"character", SWQ_STRING},
      {"integer", SWQ_INTEGER},  {"bigint", SWQ_INTEGER64},
      {"smallint", SWQ_INTEGER}, {"float", SWQ_FLOAT},
      {"numeric", SWQ_FLOAT},    {"timestamp", SWQ_TIMESTAMP},
      {"date", SWQ_DATE},        {"time", SWQ_TIME},
  };

  for (const auto& [typeName, expectedType] : validTypes) {
    swq_expr_node castNode(SWQ_CAST);
    castNode.PushSubExpression(new swq_expr_node(42));
    castNode.PushSubExpression(new swq_expr_node(typeName));
    EXPECT_EQ(expectedType, SWQCastChecker(&castNode, FALSE));
    EXPECT_EQ(expectedType, castNode.field_type);
  }

  // Casting fid column to bigint alters fid's type to integer64
  {
    swq_expr_node castNode(SWQ_CAST);
    swq_expr_node* colFid = new swq_expr_node("fid");
    colFid->eNodeType = SNT_COLUMN;
    colFid->field_type = SWQ_INTEGER;
    castNode.PushSubExpression(colFid);
    castNode.PushSubExpression(new swq_expr_node("bigint"));
    EXPECT_EQ(SWQ_INTEGER64, SWQCastChecker(&castNode, FALSE));
    EXPECT_EQ(SWQ_INTEGER64, colFid->field_type);
  }

  // Casting geometry to character or geometry
  {
    OGRPoint pt(1.0, 2.0);
    swq_expr_node castNode(SWQ_CAST);
    castNode.PushSubExpression(new swq_expr_node(&pt));
    castNode.PushSubExpression(new swq_expr_node("character"));
    EXPECT_EQ(SWQ_STRING, SWQCastChecker(&castNode, FALSE));

    swq_expr_node castGeom(SWQ_CAST);
    castGeom.PushSubExpression(new swq_expr_node(&pt));
    castGeom.PushSubExpression(new swq_expr_node("geometry"));
    EXPECT_EQ(SWQ_GEOMETRY, SWQCastChecker(&castGeom, FALSE));

    // Geometry to integer should fail
    swq_expr_node castGeomInv(SWQ_CAST);
    castGeomInv.PushSubExpression(new swq_expr_node(&pt));
    castGeomInv.PushSubExpression(new swq_expr_node("integer"));
    EXPECT_EQ(SWQ_ERROR, SWQCastChecker(&castGeomInv, FALSE));
  }

  // Casting string to geometry
  {
    swq_expr_node castNode(SWQ_CAST);
    castNode.PushSubExpression(new swq_expr_node("POINT (1 2)"));
    castNode.PushSubExpression(new swq_expr_node("geometry"));
    EXPECT_EQ(SWQ_GEOMETRY, SWQCastChecker(&castNode, FALSE));
  }

  // Casting non-string/non-geometry to geometry should fail
  {
    swq_expr_node castNode(SWQ_CAST);
    castNode.PushSubExpression(new swq_expr_node(123));
    castNode.PushSubExpression(new swq_expr_node("geometry"));
    EXPECT_EQ(SWQ_ERROR, SWQCastChecker(&castNode, FALSE));
  }

  // Unrecognized typename
  {
    swq_expr_node castNode(SWQ_CAST);
    castNode.PushSubExpression(new swq_expr_node(123));
    castNode.PushSubExpression(new swq_expr_node("invalid_type"));
    EXPECT_EQ(SWQ_ERROR, SWQCastChecker(&castNode, FALSE));
  }
}

TEST(SwqOpGeneralTest, CastEvaluatorAllTypes) {
  // Cast to INTEGER from int, int64, float, string
  {
    swq_expr_node* ret = EvalCast(SWQ_INTEGER, {new swq_expr_node(10)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(10, ret->int_value);
    delete ret;

    ret = EvalCast(SWQ_INTEGER, {new swq_expr_node(static_cast<GIntBig>(500))});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(500, ret->int_value);
    delete ret;

    ret = EvalCast(SWQ_INTEGER, {new swq_expr_node(12.8)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(12, ret->int_value);
    delete ret;

    ret = EvalCast(SWQ_INTEGER, {new swq_expr_node("42")});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(42, ret->int_value);
    delete ret;
  }

  // Cast to INTEGER64 from int, float, string
  {
    swq_expr_node* ret = EvalCast(SWQ_INTEGER64, {new swq_expr_node(100)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(100, ret->int_value);
    delete ret;

    ret = EvalCast(SWQ_INTEGER64, {new swq_expr_node(99.9)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(99, ret->int_value);
    delete ret;

    ret = EvalCast(SWQ_INTEGER64, {new swq_expr_node("1234567890123")});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1234567890123LL, ret->int_value);
    delete ret;
  }

  // Cast to FLOAT from int, float, string
  {
    swq_expr_node* ret = EvalCast(SWQ_FLOAT, {new swq_expr_node(25)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(25.0, ret->float_value);
    delete ret;

    ret = EvalCast(SWQ_FLOAT, {new swq_expr_node(12.34)});
    ASSERT_NE(nullptr, ret);
    EXPECT_DOUBLE_EQ(12.34, ret->float_value);
    delete ret;

    ret = EvalCast(SWQ_FLOAT, {new swq_expr_node("3.14159")});
    ASSERT_NE(nullptr, ret);
    EXPECT_NEAR(3.14159, ret->float_value, 1e-5);
    delete ret;
  }

  // Cast to GEOMETRY from null, geometry, WKT string, invalid WKT string, and
  // integer
  {
    // Null source
    auto* srcNull = new swq_expr_node("null");
    srcNull->is_null = 1;
    swq_expr_node* ret = EvalCast(SWQ_GEOMETRY, {srcNull});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->is_null);
    EXPECT_EQ(nullptr, ret->geometry_value);
    delete ret;

    // Clone geometry
    OGRPoint pt(3.0, 4.0);
    ret = EvalCast(SWQ_GEOMETRY, {new swq_expr_node(&pt)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->is_null);
    ASSERT_NE(nullptr, ret->geometry_value);
    EXPECT_STREQ("POINT", ret->geometry_value->getGeometryName());
    delete ret;

    // String to geometry (valid WKT)
    ret = EvalCast(SWQ_GEOMETRY, {new swq_expr_node("POINT (10 20)")});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->is_null);
    ASSERT_NE(nullptr, ret->geometry_value);
    EXPECT_STREQ("POINT", ret->geometry_value->getGeometryName());
    delete ret;

    // String to geometry (invalid WKT)
    WithQuietHandler quietHandler;
    ret = EvalCast(SWQ_GEOMETRY, {new swq_expr_node("NOT_A_WKT")});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nullptr, ret->geometry_value);
    delete ret;

    // Integer to geometry (unsupported source type)
    ret = EvalCast(SWQ_GEOMETRY, {new swq_expr_node(42)});
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nullptr, ret->geometry_value);
    delete ret;
  }

  // Cast to STRING from integer, float, geometry, and string (with width
  // truncation)
  {
    swq_expr_node* ret = EvalCast(SWQ_STRING, {new swq_expr_node(12345)});
    ASSERT_NE(nullptr, ret);
    EXPECT_STREQ("12345", ret->string_value);
    delete ret;

    ret = EvalCast(SWQ_STRING, {new swq_expr_node(1.5)});
    ASSERT_NE(nullptr, ret);
    EXPECT_STREQ("1.5", ret->string_value);
    delete ret;

    OGRPoint pt(1.0, 2.0);
    ret = EvalCast(SWQ_STRING, {new swq_expr_node(&pt)});
    ASSERT_NE(nullptr, ret);
    EXPECT_STREQ("POINT (1 2)", ret->string_value);
    delete ret;

    // Geometry with null geometry_value
    auto* srcNullGeom = new swq_expr_node(static_cast<OGRGeometry*>(nullptr));
    srcNullGeom->is_null = 0;
    ret = EvalCast(SWQ_STRING, {srcNullGeom});
    ASSERT_NE(nullptr, ret);
    EXPECT_STREQ("", ret->string_value);
    delete ret;

    // With width truncation (nSubExprCount > 2)
    ret = EvalCast(SWQ_STRING,
                   {new swq_expr_node("1234567890"),
                    new swq_expr_node("character"), new swq_expr_node(5)});
    ASSERT_NE(nullptr, ret);
    EXPECT_STREQ("12345", ret->string_value);
    delete ret;
  }
}

}  // namespace
}  // namespace autotest2
