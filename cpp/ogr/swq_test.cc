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

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

// Token constants defined by swq_parser
enum {
  YYerror = 256,
  SWQT_INTEGER_NUMBER = 258,
  SWQT_FLOAT_NUMBER = 259,
  SWQT_STRING = 260,
  SWQT_IDENTIFIER = 261,
  SWQT_IN = 262,
  SWQT_LIKE = 263,
  SWQT_ILIKE = 264,
  SWQT_ESCAPE = 265,
  SWQT_BETWEEN = 266,
  SWQT_NULL = 267,
  SWQT_IS = 268,
  SWQT_SELECT = 269,
  SWQT_LEFT = 270,
  SWQT_JOIN = 271,
  SWQT_WHERE = 272,
  SWQT_ON = 273,
  SWQT_ORDER = 274,
  SWQT_BY = 275,
  SWQT_FROM = 276,
  SWQT_AS = 277,
  SWQT_ASC = 278,
  SWQT_DESC = 279,
  SWQT_DISTINCT = 280,
  SWQT_CAST = 281,
  SWQT_UNION = 282,
  SWQT_ALL = 283,
  SWQT_LIMIT = 284,
  SWQT_OFFSET = 285,
  SWQT_EXCEPT = 286,
  SWQT_EXCLUDE = 287,
  SWQT_HIDDEN = 288,
  SWQT_VALUE_START = 289,
  SWQT_SELECT_START = 290,
  SWQT_NOT = 291,
  SWQT_OR = 292,
  SWQT_AND = 293,
  SWQT_UMINUS = 294,
  SWQT_RESERVED_KEYWORD = 295,
};

// Forward declare internal functions from swq.cpp
void swq_fixup(swq_parse_context* psParseContext);
swq_expr_node* swq_create_and_or_or(swq_op op, swq_expr_node* left,
                                    swq_expr_node* right);

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

static swq_field_type DummyChecker(swq_expr_node*, int) { return SWQ_STRING; }

static swq_expr_node* DummyEvaluator(swq_expr_node*, swq_expr_node**,
                                     const swq_evaluation_context&) {
  return new swq_expr_node("xy");
}

class DummyCustomFuncRegistrar : public swq_custom_func_registrar {
 public:
  const swq_operation* GetOperator(const char* name) override {
    static const swq_operation kOp = {"dummy_custom", SWQ_CUSTOM_FUNC,
                                      DummyEvaluator, DummyChecker};
    if (EQUAL(name, "dummy_custom")) {
      return &kOp;
    }
    return nullptr;
  }
};

TEST(SwqTest, SwqErrorContextFormatting) {
  WithQuietHandler quiet_handler;
  swq_parse_context context;

  // Short input: n < 40
  std::string short_input = "SELECT * FROM";
  context.pszInput = short_input.c_str();
  context.pszLastValid = context.pszInput + 7;
  swqerror(&context, "Syntax error short");

  // Long input: n > 40 and total length > 100
  std::string long_input =
      "SELECT very_long_column_name_1, very_long_column_name_2, "
      "very_long_column_name_3 FROM some_table WHERE some_condition";
  context.pszInput = long_input.c_str();
  context.pszLastValid = context.pszInput + 55;
  swqerror(&context, "Syntax error long");
}

TEST(SwqTest, SwqLexStartToken) {
  swq_parse_context context;
  context.nStartToken = SWQT_VALUE_START;
  swq_expr_node* node = nullptr;

  int token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_VALUE_START);
  EXPECT_EQ(context.nStartToken, 0);
  EXPECT_EQ(node, nullptr);
}

TEST(SwqTest, SwqLexWhitespaceAndEof) {
  swq_parse_context context;
  std::string input = "   \t \r \n  ";
  context.pszInput = input.c_str();
  context.pszNext = input.c_str();
  swq_expr_node* node = nullptr;

  int token = swqlex(&node, &context);
  EXPECT_EQ(token, EOF);
  EXPECT_EQ(node, nullptr);
}

TEST(SwqTest, SwqLexStringAndIdentifierQuotes) {
  swq_parse_context context;
  swq_expr_node* node = nullptr;

  // Double quotes -> SWQT_IDENTIFIER
  std::string id_str = "\"my_col_\\\"name\"";
  context.pszInput = id_str.c_str();
  context.pszNext = id_str.c_str();
  int token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_IDENTIFIER);
  ASSERT_NE(node, nullptr);
  EXPECT_STREQ(node->string_value, "my_col_\"name");
  delete node;
  node = nullptr;

  // Single quotes -> SWQT_STRING with '' escape
  std::string lit_str = "'it''s a string'";
  context.pszInput = lit_str.c_str();
  context.pszNext = lit_str.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_STRING);
  ASSERT_NE(node, nullptr);
  EXPECT_STREQ(node->string_value, "it's a string");
  delete node;
  node = nullptr;

  // Unterminated single quote -> error
  {
    WithQuietHandler quiet_handler;
    std::string unterm = "'unterminated";
    context.pszInput = unterm.c_str();
    context.pszNext = unterm.c_str();
    token = swqlex(&node, &context);
    EXPECT_EQ(token, YYerror);
    EXPECT_EQ(node, nullptr);
  }

  // Trailing backslash in double quotes -> error
  {
    WithQuietHandler quiet_handler;
    std::string trailing_bs = "\"escaped_end\\";
    context.pszInput = trailing_bs.c_str();
    context.pszNext = trailing_bs.c_str();
    token = swqlex(&node, &context);
    EXPECT_EQ(token, YYerror);
    EXPECT_EQ(node, nullptr);
  }
}

TEST(SwqTest, SwqLexNumbers) {
  swq_parse_context context;
  swq_expr_node* node = nullptr;

  // 32-bit integer
  std::string int32_str = "12345";
  context.pszInput = int32_str.c_str();
  context.pszNext = int32_str.c_str();
  int token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_INTEGER_NUMBER);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->int_value, 12345);
  delete node;
  node = nullptr;

  // 64-bit integer
  std::string int64_str = "5000000000";
  context.pszInput = int64_str.c_str();
  context.pszNext = int64_str.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_INTEGER_NUMBER);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->int_value, 5000000000LL);
  delete node;
  node = nullptr;

  // Float with decimal
  std::string float_str = "123.456";
  context.pszInput = float_str.c_str();
  context.pszNext = float_str.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_FLOAT_NUMBER);
  ASSERT_NE(node, nullptr);
  EXPECT_NEAR(node->float_value, 123.456, 1e-6);
  delete node;
  node = nullptr;

  // Float with exponent
  std::string exp_str = "1.5e-3";
  context.pszInput = exp_str.c_str();
  context.pszNext = exp_str.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_FLOAT_NUMBER);
  ASSERT_NE(node, nullptr);
  EXPECT_NEAR(node->float_value, 0.0015, 1e-6);
  delete node;
  node = nullptr;

  // Float with positive exponent
  std::string exp_pos_str = "2E+4";
  context.pszInput = exp_pos_str.c_str();
  context.pszNext = exp_pos_str.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_FLOAT_NUMBER);
  ASSERT_NE(node, nullptr);
  EXPECT_NEAR(node->float_value, 20000.0, 1e-6);
  delete node;
  node = nullptr;

  // Integer > 19 digits -> float
  std::string huge_int = "1234567890123456789012";
  context.pszInput = huge_int.c_str();
  context.pszNext = huge_int.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_FLOAT_NUMBER);
  ASSERT_NE(node, nullptr);
  delete node;
  node = nullptr;

  // Exactly 9223372036854775808 special case
  std::string max_int64_plus_1 = "9223372036854775808";
  context.pszInput = max_int64_plus_1.c_str();
  context.pszNext = max_int64_plus_1.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_FLOAT_NUMBER);
  ASSERT_NE(node, nullptr);
  EXPECT_STREQ(node->string_value, "9223372036854775808");
  delete node;
  node = nullptr;
}

TEST(SwqTest, SwqLexKeywordsAndIdentifiers) {
  swq_parse_context context;
  swq_expr_node* node = nullptr;

  const struct {
    const char* keyword;
    int expected_token;
  } kTestCases[] = {
      {"IN", SWQT_IN},
      {"LIKE", SWQT_LIKE},
      {"ILIKE", SWQT_ILIKE},
      {"ESCAPE", SWQT_ESCAPE},
      {"EXCEPT", SWQT_EXCEPT},
      {"EXCLUDE", SWQT_EXCLUDE},
      {"NULL", SWQT_NULL},
      {"IS", SWQT_IS},
      {"NOT", SWQT_NOT},
      {"AND", SWQT_AND},
      {"OR", SWQT_OR},
      {"BETWEEN", SWQT_BETWEEN},
      {"SELECT", SWQT_SELECT},
      {"LEFT", SWQT_LEFT},
      {"JOIN", SWQT_JOIN},
      {"WHERE", SWQT_WHERE},
      {"ON", SWQT_ON},
      {"ORDER", SWQT_ORDER},
      {"BY", SWQT_BY},
      {"FROM", SWQT_FROM},
      {"AS", SWQT_AS},
      {"ASC", SWQT_ASC},
      {"DESC", SWQT_DESC},
      {"DISTINCT", SWQT_DISTINCT},
      {"CAST", SWQT_CAST},
      {"UNION", SWQT_UNION},
      {"ALL", SWQT_ALL},
      {"LIMIT", SWQT_LIMIT},
      {"OFFSET", SWQT_OFFSET},
      {"HIDDEN", SWQT_HIDDEN},
      {"OUTER", SWQT_RESERVED_KEYWORD},
      {"INNER", SWQT_RESERVED_KEYWORD},
  };

  for (const auto& tc : kTestCases) {
    context.pszInput = tc.keyword;
    context.pszNext = tc.keyword;
    int token = swqlex(&node, &context);
    EXPECT_EQ(token, tc.expected_token) << "Failed for keyword " << tc.keyword;
    delete node;
    node = nullptr;
  }

  // Regular identifier with underscore and non-ASCII byte (>127)
  std::string id_with_unicode = "field_\xC3\xA9_name";
  context.pszInput = id_with_unicode.c_str();
  context.pszNext = id_with_unicode.c_str();
  int token = swqlex(&node, &context);
  EXPECT_EQ(token, SWQT_IDENTIFIER);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->string_value, id_with_unicode);
  delete node;
  node = nullptr;

  // Single-char special token
  std::string special = "+";
  context.pszInput = special.c_str();
  context.pszNext = special.c_str();
  token = swqlex(&node, &context);
  EXPECT_EQ(token, '+');
  EXPECT_EQ(node, nullptr);
}

TEST(SwqTest, SelectSummarizeErrors) {
  swq_select select_info;

  // SWQM_RECORDSET error
  select_info.query_mode = SWQM_RECORDSET;
  EXPECT_THAT(swq_select_summarize(&select_info, 0, "val", nullptr),
              HasSubstr("non-summary query"));

  select_info.query_mode = SWQM_SUMMARY_RECORD;
  // Out of range dest_column
  EXPECT_THAT(swq_select_summarize(&select_info, -1, "val", nullptr),
              HasSubstr("out of range"));
  EXPECT_THAT(swq_select_summarize(&select_info, 0, "val", nullptr),
              HasSubstr("out of range"));

  // Add column definition with SWQCF_NONE and no distinct
  swq_col_def col{};
  col.col_func = SWQCF_NONE;
  col.distinct_flag = 0;
  select_info.column_defs.push_back(col);
  EXPECT_EQ(swq_select_summarize(&select_info, 0, "val", nullptr), nullptr);

  // SWQM_DISTINCT_LIST with multiple order specs
  select_info.query_mode = SWQM_DISTINCT_LIST;
  select_info.column_defs[0].distinct_flag = 1;
  select_info.order_specs = 2;
  EXPECT_THAT(swq_select_summarize(&select_info, 0, "val", nullptr),
              HasSubstr("Can't ORDER BY a DISTINCT list by more than one"));
  select_info.order_specs = 0;

  // SWQM_DISTINCT_LIST where order_def field != col_def field
  select_info.order_specs = 1;
  swq_order_def odef{};
  odef.field_index = 5;
  select_info.order_defs = &odef;
  select_info.column_defs[0].field_index = 0;
  EXPECT_THAT(swq_select_summarize(&select_info, 0, "val", nullptr),
              HasSubstr("Only selected DISTINCT field can be used"));
  select_info.order_defs = nullptr;
  select_info.order_specs = 0;

  // SWQCF_CUSTOM error
  select_info.query_mode = SWQM_SUMMARY_RECORD;
  select_info.column_defs[0].col_func = SWQCF_CUSTOM;
  select_info.column_defs[0].distinct_flag = 0;
  EXPECT_THAT(swq_select_summarize(&select_info, 0, "val", nullptr),
              HasSubstr("custom field function"));
}

TEST(SwqTest, SelectSummarizeDistinctValues) {
  // Test distinct values with unsorted and sorted paths
  for (int order_spec : {0, 1}) {
    for (int asc : {1, 0}) {
      for (swq_field_type ftype : {SWQ_INTEGER, SWQ_FLOAT, SWQ_STRING}) {
        swq_select select_info;
        select_info.query_mode = SWQM_DISTINCT_LIST;

        swq_col_def col{};
        col.col_func = SWQCF_NONE;
        col.distinct_flag = 1;
        col.field_index = 0;
        col.field_type = ftype;
        select_info.column_defs.push_back(col);

        swq_order_def odef{};
        odef.field_index = 0;
        odef.ascending_flag = asc;
        if (order_spec > 0) {
          select_info.order_specs = 1;
          select_info.order_defs = &odef;
        }

        if (ftype == SWQ_INTEGER) {
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "10", nullptr),
                    nullptr);
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "2", nullptr),
                    nullptr);
          EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, nullptr),
                    nullptr);  // Null value
          // Duplicate
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "10", nullptr),
                    nullptr);
        } else if (ftype == SWQ_FLOAT) {
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "10.5", nullptr),
                    nullptr);
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "2.3", nullptr),
                    nullptr);
          EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, nullptr),
                    nullptr);
        } else {
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "beta", nullptr),
                    nullptr);
          EXPECT_EQ(swq_select_summarize(&select_info, 0, "alpha", nullptr),
                    nullptr);
          EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, nullptr),
                    nullptr);
        }

        select_info.order_defs = nullptr;
        select_info.order_specs = 0;
      }
      if (order_spec == 0) break;
    }
  }
}

TEST(SwqTest, SelectSummarizeMinMax) {
  swq_select select_info;
  select_info.query_mode = SWQM_SUMMARY_RECORD;

  swq_col_def col_min{};
  col_min.col_func = SWQCF_MIN;
  col_min.distinct_flag = 0;
  select_info.column_defs.push_back(col_min);

  swq_col_def col_max{};
  col_max.col_func = SWQCF_MAX;
  col_max.distinct_flag = 0;
  select_info.column_defs.push_back(col_max);

  // Numeric MIN / MAX
  double val1 = 10.0;
  double val2 = 5.0;
  double val3 = 20.0;
  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &val1), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &val2), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &val3), nullptr);
  EXPECT_DOUBLE_EQ(select_info.column_summary[0].min, 5.0);

  EXPECT_EQ(swq_select_summarize(&select_info, 1, nullptr, &val1), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 1, nullptr, &val2), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 1, nullptr, &val3), nullptr);
  EXPECT_DOUBLE_EQ(select_info.column_summary[1].max, 20.0);

  // String MIN / MAX
  swq_select select_str;
  select_str.query_mode = SWQM_SUMMARY_RECORD;
  select_str.column_defs.push_back(col_min);
  select_str.column_defs.push_back(col_max);

  EXPECT_EQ(swq_select_summarize(&select_str, 0, "bravo", nullptr), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_str, 0, "alpha", nullptr), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_str, 0, "charlie", nullptr), nullptr);
  EXPECT_EQ(select_str.column_summary[0].osMin, "alpha");

  EXPECT_EQ(swq_select_summarize(&select_str, 1, "bravo", nullptr), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_str, 1, "alpha", nullptr), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_str, 1, "charlie", nullptr), nullptr);
  EXPECT_EQ(select_str.column_summary[1].osMax, "charlie");
}

TEST(SwqTest, SelectSummarizeSumAvgCountStddev) {
  swq_select select_info;
  select_info.query_mode = SWQM_SUMMARY_RECORD;

  swq_col_def col_sum{};
  col_sum.col_func = SWQCF_SUM;
  col_sum.distinct_flag = 0;
  col_sum.field_type = SWQ_FLOAT;
  select_info.column_defs.push_back(col_sum);

  swq_col_def col_count{};
  col_count.col_func = SWQCF_COUNT;
  col_count.distinct_flag = 0;
  select_info.column_defs.push_back(col_count);

  swq_col_def col_stddev{};
  col_stddev.col_func = SWQCF_STDDEV_SAMP;
  col_stddev.distinct_flag = 0;
  col_stddev.field_type = SWQ_FLOAT;
  select_info.column_defs.push_back(col_stddev);

  double v1 = 2.0;
  double v2 = 100.0;  // test fabs(acc) < fabs(new_val)
  double v3 = 1.0;    // test fabs(acc) >= fabs(new_val)
  double inf = std::numeric_limits<double>::infinity();

  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &v1), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &v2), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &v3), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 0, nullptr, &inf), nullptr);

  // COUNT
  EXPECT_EQ(swq_select_summarize(&select_info, 1, nullptr, &v1), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 1, "text", nullptr), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 1, nullptr, nullptr), nullptr);
  EXPECT_EQ(select_info.column_summary[1].count, 2);

  // STDDEV
  EXPECT_EQ(swq_select_summarize(&select_info, 2, nullptr, &v1), nullptr);
  EXPECT_EQ(swq_select_summarize(&select_info, 2, nullptr, &v2), nullptr);

  // Date/Time/Timestamp SUM and STDDEV
  swq_select select_date;
  select_date.query_mode = SWQM_SUMMARY_RECORD;

  swq_col_def col_date_sum{};
  col_date_sum.col_func = SWQCF_SUM;
  col_date_sum.distinct_flag = 0;
  col_date_sum.field_type = SWQ_TIMESTAMP;
  select_date.column_defs.push_back(col_date_sum);

  swq_col_def col_date_stddev{};
  col_date_stddev.col_func = SWQCF_STDDEV_POP;
  col_date_stddev.distinct_flag = 0;
  col_date_stddev.field_type = SWQ_TIMESTAMP;
  select_date.column_defs.push_back(col_date_stddev);

  EXPECT_EQ(
      swq_select_summarize(&select_date, 0, "2020/01/01 12:00:00", nullptr),
      nullptr);
  EXPECT_EQ(
      swq_select_summarize(&select_date, 1, "2020/01/01 12:00:00", nullptr),
      nullptr);

  // Error when calling SUM/STDDEV with string field
  swq_select select_err;
  select_err.query_mode = SWQM_SUMMARY_RECORD;
  swq_col_def col_str_sum{};
  col_str_sum.col_func = SWQCF_SUM;
  col_str_sum.field_type = SWQ_STRING;
  select_err.column_defs.push_back(col_str_sum);

  swq_col_def col_str_stddev{};
  col_str_stddev.col_func = SWQCF_STDDEV_POP;
  col_str_stddev.field_type = SWQ_STRING;
  select_err.column_defs.push_back(col_str_stddev);

  EXPECT_THAT(swq_select_summarize(&select_err, 0, "not_date", nullptr),
              HasSubstr("AVG()/SUM() called on unexpected field type"));
  EXPECT_THAT(swq_select_summarize(&select_err, 1, "not_date", nullptr),
              HasSubstr("STDDEV() called on unexpected field type"));
}

TEST(SwqTest, IdentifyFieldBasicAndQuotingFallback) {
  swq_field_list field_list;
  char col1[] = "col1";
  char col2[] = "COL2";
  char dotted[] = "my_table.dotted_field";
  char* names[] = {col1, col2, dotted};
  swq_field_type types[] = {SWQ_INTEGER, SWQ_FLOAT, SWQ_STRING};
  int ids[] = {100, 200, 300};

  field_list.count = 3;
  field_list.names = names;
  field_list.types = types;
  field_list.ids = ids;
  field_list.table_ids = nullptr;
  field_list.table_count = 0;
  field_list.table_defs = nullptr;

  swq_field_type out_type = SWQ_OTHER;
  int table_id = -1;

  // Exact match (pass 0)
  int fid =
      swq_identify_field(nullptr, "col1", &field_list, &out_type, &table_id);
  EXPECT_EQ(fid, 100);
  EXPECT_EQ(out_type, SWQ_INTEGER);

  // Case-insensitive match (pass 1)
  fid = swq_identify_field(nullptr, "col2", &field_list, &out_type, &table_id);
  EXPECT_EQ(fid, 200);
  EXPECT_EQ(out_type, SWQ_FLOAT);

  // Field not found
  fid = swq_identify_field(nullptr, "nonexistent", &field_list, &out_type,
                           &table_id);
  EXPECT_EQ(fid, -1);
  EXPECT_EQ(out_type, SWQ_OTHER);

  // When field_list.types and ids are null
  field_list.types = nullptr;
  field_list.ids = nullptr;
  fid = swq_identify_field(nullptr, "col1", &field_list, &out_type, &table_id);
  EXPECT_EQ(fid, 0);
  EXPECT_EQ(out_type, SWQ_OTHER);

  // Quoting error fallback: table_name="my_table", field_token="dotted_field"
  // should match "my_table.dotted_field" with warning
  {
    WithQuietHandler quiet_handler;
    fid = swq_identify_field("my_table", "dotted_field", &field_list, &out_type,
                             &table_id);
    EXPECT_EQ(fid, 2);
  }

  // Quoting error fallback: table_name="", field_token="my_table.dotted_field"
  // with table definitions enabled
  char t_alias[] = "my_table";
  swq_table_def tdef;
  tdef.table_alias = t_alias;
  tdef.table_name = t_alias;
  tdef.data_source = nullptr;
  int table_ids[] = {0, 0, 0};

  field_list.table_count = 1;
  field_list.table_defs = &tdef;
  field_list.table_ids = table_ids;

  char plain_field[] = "dotted_field";
  char* names2[] = {col1, col2, plain_field};
  field_list.names = names2;

  {
    WithQuietHandler quiet_handler;
    fid = swq_identify_field(nullptr, "my_table.dotted_field", &field_list,
                             &out_type, &table_id);
    EXPECT_EQ(fid, 2);
  }

  // When OGR_SQL_STRICT is TRUE, quoting error fallback is disabled
  CPLSetConfigOption("OGR_SQL_STRICT", "TRUE");
  fid = swq_identify_field(nullptr, "my_table.non_existent_strict", &field_list,
                           &out_type, &table_id);
  EXPECT_EQ(fid, -1);
  CPLSetConfigOption("OGR_SQL_STRICT", nullptr);
}

TEST(SwqTest, CreateAndOrOr) {
  // Case 1: left and right both have same op
  auto left1 = new swq_expr_node(SWQ_AND);
  left1->PushSubExpression(new swq_expr_node(1));
  left1->PushSubExpression(new swq_expr_node(2));

  auto right1 = new swq_expr_node(SWQ_AND);
  right1->PushSubExpression(new swq_expr_node(3));
  right1->PushSubExpression(new swq_expr_node(4));

  auto res1 = swq_create_and_or_or(SWQ_AND, left1, right1);
  ASSERT_NE(res1, nullptr);
  EXPECT_EQ(res1->nSubExprCount, 4);
  delete res1;

  // Case 2: left has same op, right has different op
  auto left2 = new swq_expr_node(SWQ_OR);
  left2->PushSubExpression(new swq_expr_node(1));
  left2->PushSubExpression(new swq_expr_node(2));
  auto right2 = new swq_expr_node(SWQ_EQ);

  auto res2 = swq_create_and_or_or(SWQ_OR, left2, right2);
  ASSERT_NE(res2, nullptr);
  EXPECT_EQ(res2->nSubExprCount, 3);
  delete res2;

  // Case 3: right has same op, left has different op
  auto left3 = new swq_expr_node(SWQ_EQ);
  auto right3 = new swq_expr_node(SWQ_OR);
  right3->PushSubExpression(new swq_expr_node(1));
  right3->PushSubExpression(new swq_expr_node(2));

  auto res3 = swq_create_and_or_or(SWQ_OR, left3, right3);
  ASSERT_NE(res3, nullptr);
  EXPECT_EQ(res3->nSubExprCount, 3);
  delete res3;

  // Case 4: neither has same op
  auto left4 = new swq_expr_node(SWQ_EQ);
  auto right4 = new swq_expr_node(SWQ_NE);

  auto res4 = swq_create_and_or_or(SWQ_AND, left4, right4);
  ASSERT_NE(res4, nullptr);
  EXPECT_EQ(res4->nSubExprCount, 2);
  delete res4;
}

TEST(SwqTest, Fixup) {
  swq_parse_context context;
  context.poRoot = new swq_expr_node(SWQ_AND);
  context.poRoot->PushSubExpression(new swq_expr_node(1));
  context.poRoot->PushSubExpression(new swq_expr_node(2));
  context.poRoot->PushSubExpression(new swq_expr_node(3));

  swq_select sel1;
  sel1.where_expr = new swq_expr_node(SWQ_OR);
  sel1.where_expr->PushSubExpression(new swq_expr_node(4));
  sel1.where_expr->PushSubExpression(new swq_expr_node(5));
  sel1.where_expr->PushSubExpression(new swq_expr_node(6));

  auto sel2 = new swq_select();
  sel2->where_expr = new swq_expr_node(SWQ_AND);
  sel2->where_expr->PushSubExpression(new swq_expr_node(7));
  sel2->where_expr->PushSubExpression(new swq_expr_node(8));

  sel1.poOtherSelect = sel2;
  context.poCurSelect = &sel1;

  swq_fixup(&context);

  delete context.poRoot;
}

TEST(SwqTest, ExprCompileAndCompile2) {
  char col_a[] = "a";
  char col_b[] = "b";
  char* names[] = {col_a, col_b};
  swq_field_type types[] = {SWQ_INTEGER, SWQ_INTEGER};

  swq_expr_node* expr = nullptr;

  // Valid compilation
  CPLErr err = swq_expr_compile("a = 1 AND b = 2", 2, names, types,
                                /*bCheck=*/1, nullptr, &expr);
  EXPECT_EQ(err, CE_None);
  ASSERT_NE(expr, nullptr);
  delete expr;
  expr = nullptr;

  // Compilation with custom function
  DummyCustomFuncRegistrar custom_registrar;
  err = swq_expr_compile("dummy_custom('x', 'y') = 'xy'", 2, names, types,
                         /*bCheck=*/1, &custom_registrar, &expr);
  EXPECT_EQ(err, CE_None);
  ASSERT_NE(expr, nullptr);
  delete expr;
  expr = nullptr;

  // Syntax error
  {
    WithQuietHandler quiet_handler;
    err =
        swq_expr_compile("a = ", 2, names, types, /*bCheck=*/1, nullptr, &expr);
    EXPECT_EQ(err, CE_Failure);
    EXPECT_EQ(expr, nullptr);
  }

  // Type check failure: a is integer, 'str' is string
  {
    WithQuietHandler quiet_handler;
    err = swq_expr_compile("a = 'str'", 2, names, types, /*bCheck=*/1, nullptr,
                           &expr);
    EXPECT_EQ(err, CE_Failure);
    EXPECT_EQ(expr, nullptr);
  }
}

TEST(SwqTest, ReservedKeywords) {
  EXPECT_TRUE(swq_is_reserved_keyword("SELECT"));
  EXPECT_TRUE(swq_is_reserved_keyword("where"));
  EXPECT_TRUE(swq_is_reserved_keyword("AND"));
  EXPECT_TRUE(swq_is_reserved_keyword("Or"));
  EXPECT_TRUE(swq_is_reserved_keyword("BETWEEN"));
  EXPECT_TRUE(swq_is_reserved_keyword("UNION"));
  EXPECT_FALSE(swq_is_reserved_keyword("MY_TABLE"));
  EXPECT_FALSE(swq_is_reserved_keyword("FIELD_NAME"));
}

TEST(SwqTest, FieldTypeToString) {
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_INTEGER), "integer");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_INTEGER64), "bigint");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_FLOAT), "float");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_STRING), "string");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_BOOLEAN), "boolean");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_DATE), "date");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_TIME), "time");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_TIMESTAMP), "timestamp");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_GEOMETRY), "geometry");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_NULL), "null");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_OTHER), "unknown");
  EXPECT_STREQ(SWQFieldTypeToString(SWQ_ERROR), "unknown");
}

}  // namespace
}  // namespace autotest2
