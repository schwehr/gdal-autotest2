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
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class DummyCustomFuncRegistrar : public swq_custom_func_registrar {
 public:
  const swq_operation* GetOperator(const char* name) override {
    static const swq_operation op = {
        "MY_CUSTOM", SWQ_CUSTOM_FUNC,
        [](swq_expr_node*, swq_expr_node**, const swq_evaluation_context&)
            -> swq_expr_node* { return new swq_expr_node("custom_eval"); },
        [](swq_expr_node*, int) -> swq_field_type { return SWQ_STRING; }};
    if (name && strcmp(name, "MY_CUSTOM") == 0) return &op;
    return nullptr;
  }
};

TEST(SwqExprNodeTest, ConstructorsAndProperties) {
  // Default
  {
    swq_expr_node node;
    EXPECT_EQ(node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(node.field_type, SWQ_INTEGER);
    EXPECT_EQ(node.int_value, 0);
    EXPECT_FALSE(node.HasReachedMaxDepth());
  }

  // int
  {
    swq_expr_node node(42);
    EXPECT_EQ(node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(node.field_type, SWQ_INTEGER);
    EXPECT_EQ(node.int_value, 42);
  }

  // GIntBig
  {
    swq_expr_node node(static_cast<GIntBig>(123456789012345LL));
    EXPECT_EQ(node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(node.field_type, SWQ_INTEGER64);
    EXPECT_EQ(node.int_value, 123456789012345LL);
  }

  // double
  {
    swq_expr_node node(3.14159);
    EXPECT_EQ(node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(node.field_type, SWQ_FLOAT);
    EXPECT_DOUBLE_EQ(node.float_value, 3.14159);
  }

  // const char*
  {
    swq_expr_node node("hello");
    EXPECT_EQ(node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(node.field_type, SWQ_STRING);
    EXPECT_FALSE(node.is_null);
    EXPECT_STREQ(node.string_value, "hello");

    swq_expr_node null_node(static_cast<const char*>(nullptr));
    EXPECT_EQ(null_node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(null_node.field_type, SWQ_STRING);
    EXPECT_TRUE(null_node.is_null);
    EXPECT_STREQ(null_node.string_value, "");
  }

  // OGRGeometry*
  {
    OGRPoint pt(1.0, 2.0);
    swq_expr_node node(&pt);
    EXPECT_EQ(node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(node.field_type, SWQ_GEOMETRY);
    EXPECT_FALSE(node.is_null);
    ASSERT_NE(node.geometry_value, nullptr);
    EXPECT_TRUE(node.geometry_value->Equals(&pt));

    swq_expr_node null_geom_node(static_cast<OGRGeometry*>(nullptr));
    EXPECT_EQ(null_geom_node.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(null_geom_node.field_type, SWQ_GEOMETRY);
    EXPECT_TRUE(null_geom_node.is_null);
    EXPECT_EQ(null_geom_node.geometry_value, nullptr);
  }

  // swq_op
  {
    swq_expr_node node(SWQ_ADD);
    EXPECT_EQ(node.eNodeType, SNT_OPERATION);
    EXPECT_EQ(node.nOperation, SWQ_ADD);
    EXPECT_EQ(node.nSubExprCount, 0);
  }
}

TEST(SwqExprNodeTest, CopyAndMove) {
  swq_expr_node node(SWQ_EQ);
  node.table_name = CPLStrdup("tab");
  node.string_value = CPLStrdup("col");
  OGRPoint pt(5, 10);
  node.geometry_value = pt.clone();
  node.PushSubExpression(new swq_expr_node(10));
  node.PushSubExpression(new swq_expr_node(20));

  // Copy constructor
  swq_expr_node copy_node(node);
  EXPECT_TRUE(copy_node == node);
  EXPECT_STREQ(copy_node.table_name, "tab");
  EXPECT_STREQ(copy_node.string_value, "col");
  ASSERT_NE(copy_node.geometry_value, nullptr);
  EXPECT_TRUE(copy_node.geometry_value->Equals(&pt));
  EXPECT_EQ(copy_node.nSubExprCount, 2);

  // Copy assignment
  swq_expr_node assigned;
  assigned = node;
  EXPECT_TRUE(assigned == node);

  // Self assignment
  assigned = assigned;
  EXPECT_TRUE(assigned == node);

  // Move constructor
  swq_expr_node moved(std::move(copy_node));
  EXPECT_TRUE(moved == node);

  // Move assignment
  swq_expr_node move_assigned;
  move_assigned = std::move(assigned);
  EXPECT_TRUE(move_assigned == node);

  // Self move assignment
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
  move_assigned = std::move(move_assigned);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  EXPECT_TRUE(move_assigned == node);

  // Clone
  std::unique_ptr<swq_expr_node> cloned(node.Clone());
  ASSERT_NE(cloned, nullptr);
  EXPECT_TRUE(*cloned == node);
}

TEST(SwqExprNodeTest, EqualityOperatorBranches) {
  swq_expr_node a(10);
  swq_expr_node b(10);
  EXPECT_TRUE(a == b);

  // eNodeType mismatch
  b.eNodeType = SNT_COLUMN;
  EXPECT_FALSE(a == b);
  b.eNodeType = SNT_CONSTANT;

  // field_type mismatch
  b.field_type = SWQ_FLOAT;
  EXPECT_FALSE(a == b);
  b.field_type = SWQ_INTEGER;

  // nOperation mismatch
  b.nOperation = SWQ_SUBTRACT;
  EXPECT_FALSE(a == b);
  b.nOperation = SWQ_OR;

  // field_index mismatch
  b.field_index = 5;
  EXPECT_FALSE(a == b);
  b.field_index = 0;

  // table_index mismatch
  b.table_index = 2;
  EXPECT_FALSE(a == b);
  b.table_index = 0;

  // is_null mismatch
  b.is_null = 1;
  EXPECT_FALSE(a == b);
  b.is_null = 0;

  // int_value mismatch
  b.int_value = 99;
  EXPECT_FALSE(a == b);
  b.int_value = 10;

  // float_value mismatch
  b.float_value = 1.0;
  EXPECT_FALSE(a == b);
  b.float_value = 0.0;

  // bHidden mismatch
  b.bHidden = true;
  EXPECT_FALSE(a == b);
  b.bHidden = false;

  // nSubExprCount mismatch
  b.PushSubExpression(new swq_expr_node(1));
  EXPECT_FALSE(a == b);

  // Subexpression inequality
  swq_expr_node op1(SWQ_AND);
  op1.PushSubExpression(new swq_expr_node(1));
  swq_expr_node op2(SWQ_AND);
  op2.PushSubExpression(new swq_expr_node(2));
  EXPECT_FALSE(op1 == op2);

  // table_name branches
  swq_expr_node t1;
  swq_expr_node t2;
  t1.table_name = CPLStrdup("tab1");
  EXPECT_FALSE(t1 == t2);
  EXPECT_FALSE(t2 == t1);
  t2.table_name = CPLStrdup("tab2");
  EXPECT_FALSE(t1 == t2);
  CPLFree(t2.table_name);
  t2.table_name = CPLStrdup("tab1");
  EXPECT_TRUE(t1 == t2);

  // string_value branches
  swq_expr_node s1;
  swq_expr_node s2;
  s1.string_value = CPLStrdup("str1");
  EXPECT_FALSE(s1 == s2);
  EXPECT_FALSE(s2 == s1);
  s2.string_value = CPLStrdup("str2");
  EXPECT_FALSE(s1 == s2);
  CPLFree(s2.string_value);
  s2.string_value = CPLStrdup("str1");
  EXPECT_TRUE(s1 == s2);

  // geometry_value branches
  swq_expr_node g1;
  swq_expr_node g2;
  OGRPoint pt1(1, 2);
  OGRPoint pt2(3, 4);
  g1.geometry_value = pt1.clone();
  EXPECT_FALSE(g1 == g2);
  EXPECT_FALSE(g2 == g1);
  g2.geometry_value = pt2.clone();
  EXPECT_FALSE(g1 == g2);
  delete g2.geometry_value;
  g2.geometry_value = pt1.clone();
  EXPECT_TRUE(g1 == g2);
}

TEST(SwqExprNodeTest, SubExpressionsAndDepth) {
  swq_expr_node op(SWQ_AND);
  auto* c1 = new swq_expr_node(1);
  auto* c2 = new swq_expr_node(2);
  auto* c3 = new swq_expr_node(3);

  op.PushSubExpression(c1);
  op.PushSubExpression(c2);
  op.PushSubExpression(c3);
  EXPECT_EQ(op.nSubExprCount, 3);
  EXPECT_EQ(op.papoSubExpr[0]->int_value, 1);
  EXPECT_EQ(op.papoSubExpr[1]->int_value, 2);
  EXPECT_EQ(op.papoSubExpr[2]->int_value, 3);

  op.ReverseSubExpressions();
  EXPECT_EQ(op.papoSubExpr[0]->int_value, 3);
  EXPECT_EQ(op.papoSubExpr[1]->int_value, 2);
  EXPECT_EQ(op.papoSubExpr[2]->int_value, 1);

  // HasReachedMaxDepth
  EXPECT_FALSE(op.HasReachedMaxDepth());
  op.nDepth = 128;
  EXPECT_TRUE(op.HasReachedMaxDepth());

  // MarkAsTimestamp
  swq_expr_node ts("2026-01-01 12:00:00");
  ts.MarkAsTimestamp();
  EXPECT_EQ(ts.field_type, SWQ_TIMESTAMP);
}

TEST(SwqExprNodeTest, CheckMethod) {
  const char* names[] = {"fid", "name", "sec_field"};
  swq_field_type types[] = {SWQ_INTEGER, SWQ_STRING, SWQ_FLOAT};
  int table_ids[] = {0, 0, 1};
  int ids[] = {0, 1, 0};
  swq_table_def tables[2];
  tables[0].data_source = nullptr;
  tables[0].table_name = const_cast<char*>("main_tab");
  tables[0].table_alias = const_cast<char*>("main_tab");
  tables[1].data_source = nullptr;
  tables[1].table_name = const_cast<char*>("sec_tab");
  tables[1].table_alias = const_cast<char*>("sec_tab");

  swq_field_list fl;
  fl.count = 3;
  fl.names = const_cast<char**>(names);
  fl.types = types;
  fl.table_ids = table_ids;
  fl.ids = ids;
  fl.table_count = 2;
  fl.table_defs = tables;

  DummyCustomFuncRegistrar custom_reg;

  // SNT_CONSTANT
  swq_expr_node const_node(42);
  EXPECT_EQ(const_node.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_INTEGER);

  // SNT_COLUMN - unresolved in primary table
  swq_expr_node col_node;
  col_node.eNodeType = SNT_COLUMN;
  col_node.field_index = -1;
  col_node.string_value = CPLStrdup("name");
  EXPECT_EQ(col_node.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_STRING);
  EXPECT_EQ(col_node.field_index, 1);

  // SNT_COLUMN - already resolved
  EXPECT_EQ(col_node.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_STRING);

  // SNT_COLUMN - unresolved unknown field
  {
    WithQuietHandler quiet_handler;
    swq_expr_node unknown_col;
    unknown_col.eNodeType = SNT_COLUMN;
    unknown_col.field_index = -1;
    unknown_col.string_value = CPLStrdup("nonexistent");
    EXPECT_EQ(unknown_col.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_ERROR);

    swq_expr_node unknown_tab_col;
    unknown_tab_col.eNodeType = SNT_COLUMN;
    unknown_tab_col.field_index = -1;
    unknown_tab_col.table_name = CPLStrdup("bad_table");
    unknown_tab_col.string_value = CPLStrdup("nonexistent");
    EXPECT_EQ(unknown_tab_col.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_ERROR);
  }

  // SNT_COLUMN - secondary table disallowed vs allowed
  {
    WithQuietHandler quiet_handler;
    swq_expr_node sec_col;
    sec_col.eNodeType = SNT_COLUMN;
    sec_col.field_index = -1;
    sec_col.string_value = CPLStrdup("sec_field");
    EXPECT_EQ(sec_col.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_ERROR);

    swq_expr_node sec_col_allowed;
    sec_col_allowed.eNodeType = SNT_COLUMN;
    sec_col_allowed.field_index = -1;
    sec_col_allowed.string_value = CPLStrdup("sec_field");
    EXPECT_EQ(sec_col_allowed.Check(&fl, TRUE, FALSE, &custom_reg), SWQ_FLOAT);
  }

  // SNT_OPERATION - standard op (SWQ_ADD with two integers)
  {
    swq_expr_node add_node(SWQ_ADD);
    add_node.PushSubExpression(new swq_expr_node(10));
    add_node.PushSubExpression(new swq_expr_node(20));
    EXPECT_EQ(add_node.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_INTEGER);
  }

  // SNT_OPERATION - custom func success
  {
    swq_expr_node custom_node(SWQ_CUSTOM_FUNC);
    custom_node.string_value = CPLStrdup("MY_CUSTOM");
    EXPECT_EQ(custom_node.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_STRING);
  }

  // SNT_OPERATION - custom func failure (not found or null registrar)
  {
    WithQuietHandler quiet_handler;
    swq_expr_node unknown_func(SWQ_CUSTOM_FUNC);
    unknown_func.string_value = CPLStrdup("UNKNOWN_FUNC");
    EXPECT_EQ(unknown_func.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_ERROR);

    swq_expr_node no_reg_func(SWQ_CUSTOM_FUNC);
    no_reg_func.string_value = CPLStrdup("MY_CUSTOM");
    EXPECT_EQ(no_reg_func.Check(&fl, FALSE, FALSE, nullptr), SWQ_ERROR);
  }

  // SNT_OPERATION - unknown op code
  {
    WithQuietHandler quiet_handler;
    swq_expr_node invalid_op(SWQ_ARGUMENT_LIST);
    EXPECT_EQ(invalid_op.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_ERROR);
  }

  // SNT_OPERATION - subexpression check fails
  {
    WithQuietHandler quiet_handler;
    swq_expr_node bad_sub(SWQ_ADD);
    auto* child = new swq_expr_node();
    child->eNodeType = SNT_COLUMN;
    child->field_index = -1;
    child->string_value = CPLStrdup("missing");
    bad_sub.PushSubExpression(child);
    EXPECT_EQ(bad_sub.Check(&fl, FALSE, FALSE, &custom_reg), SWQ_ERROR);
  }
}

TEST(SwqExprNodeTest, DumpMethod) {
  FILE* tmp = std::tmpfile();
  ASSERT_NE(tmp, nullptr);

  // Column
  swq_expr_node col_node;
  col_node.eNodeType = SNT_COLUMN;
  col_node.field_index = 3;
  col_node.Dump(tmp, 0);

  // Integer constants
  swq_expr_node int_node(42);
  int_node.Dump(tmp, 1);
  swq_expr_node i64_node(static_cast<GIntBig>(1000000000LL));
  i64_node.Dump(tmp, 1);
  swq_expr_node bool_node(1);
  bool_node.field_type = SWQ_BOOLEAN;
  bool_node.Dump(tmp, 1);

  // Float constant
  swq_expr_node float_node(1.2345);
  float_node.Dump(tmp, 1);

  // Geometry constant (null and non-null)
  swq_expr_node null_geom(static_cast<OGRGeometry*>(nullptr));
  null_geom.Dump(tmp, 1);
  OGRPoint pt(1, 2);
  swq_expr_node valid_geom(&pt);
  valid_geom.Dump(tmp, 1);

  // String constant
  swq_expr_node str_node("sample text");
  str_node.Dump(tmp, 1);

  // Operation (standard op with subexpressions)
  swq_expr_node op_node(SWQ_ADD);
  op_node.PushSubExpression(new swq_expr_node(1));
  op_node.PushSubExpression(new swq_expr_node(2));
  op_node.Dump(tmp, 0);

  // Operation with no op_def (e.g. custom function)
  swq_expr_node cust_op(SWQ_CUSTOM_FUNC);
  cust_op.string_value = CPLStrdup("CUSTOM_OP");
  cust_op.Dump(tmp, 0);

  std::fclose(tmp);
}

TEST(SwqExprNodeTest, QuotingMethods) {
  // Quote
  EXPECT_EQ(swq_expr_node::Quote("hello"), "'hello'");
  EXPECT_EQ(swq_expr_node::Quote("it's", '\''), "'it''s'");
  EXPECT_EQ(swq_expr_node::Quote("col", '"'), "\"col\"");

  // QuoteIfNecessary
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("_private"), "'_private'");
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("*"), "*");
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("my_col"), "my_col");
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("my.col"), "'my.col'");
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("has space"), "'has space'");
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("SELECT"), "'SELECT'");
  EXPECT_EQ(swq_expr_node::QuoteIfNecessary("WHERE"), "'WHERE'");
}

TEST(SwqExprNodeTest, UnparseConstants) {
  // Null constant
  swq_expr_node null_node;
  null_node.is_null = 1;
  char* str = null_node.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "NULL");
  CPLFree(str);

  // Integer constants
  swq_expr_node i_node(123);
  str = i_node.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "123");
  CPLFree(str);

  swq_expr_node i64_node(static_cast<GIntBig>(9876543210LL));
  str = i64_node.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "9876543210");
  CPLFree(str);

  swq_expr_node b_node(1);
  b_node.field_type = SWQ_BOOLEAN;
  str = b_node.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "1");
  CPLFree(str);

  // Float constants (integer-like float and scientific)
  swq_expr_node f_node(5.0);
  str = f_node.Unparse(nullptr, '"');
  EXPECT_THAT(str, HasSubstr("."));
  CPLFree(str);

  swq_expr_node f_sci(1.5e-10);
  str = f_sci.Unparse(nullptr, '"');
  EXPECT_THAT(str, HasSubstr("e"));
  CPLFree(str);

  // String constant
  swq_expr_node s_node("test_str");
  str = s_node.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "'test_str'");
  CPLFree(str);
}

TEST(SwqExprNodeTest, UnparseColumns) {
  // Column without field_list, no table name
  swq_expr_node col1;
  col1.eNodeType = SNT_COLUMN;
  col1.string_value = CPLStrdup("colA");
  char* str = col1.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "colA");
  CPLFree(str);

  // Column without field_list, with table name
  swq_expr_node col2;
  col2.eNodeType = SNT_COLUMN;
  col2.table_name = CPLStrdup("tab");
  col2.string_value = CPLStrdup("colB");
  str = col2.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "tab.colB");
  CPLFree(str);

  // Column with field_list and table_index > 0
  const char* names[] = {"id", "alt_id", "val"};
  swq_field_type types[] = {SWQ_INTEGER, SWQ_INTEGER, SWQ_FLOAT};
  int table_ids[] = {0, 1, 1};
  int ids[] = {0, 0, 1};
  swq_table_def tables[2];
  tables[0].data_source = nullptr;
  tables[0].table_name = const_cast<char*>("t0");
  tables[0].table_alias = const_cast<char*>("t0");
  tables[1].data_source = nullptr;
  tables[1].table_name = const_cast<char*>("t1");
  tables[1].table_alias = const_cast<char*>("t1");
  swq_field_list fl;
  fl.count = 3;
  fl.names = const_cast<char**>(names);
  fl.types = types;
  fl.table_ids = table_ids;
  fl.ids = ids;
  fl.table_count = 2;
  fl.table_defs = tables;

  swq_expr_node col3;
  col3.eNodeType = SNT_COLUMN;
  col3.field_index = 0;
  col3.table_index = 1;
  str = col3.Unparse(&fl, '"');
  EXPECT_STREQ(str, "t1.alt_id");
  CPLFree(str);

  // Column with field_list and table_index == 0
  swq_expr_node col4;
  col4.eNodeType = SNT_COLUMN;
  col4.field_index = 0;
  col4.table_index = 0;
  str = col4.Unparse(&fl, '"');
  EXPECT_STREQ(str, "id");
  CPLFree(str);

  // Column not found in field_list (empty result branch)
  swq_expr_node col5;
  col5.eNodeType = SNT_COLUMN;
  col5.field_index = 99;
  col5.table_index = 0;
  str = col5.Unparse(&fl, '"');
  EXPECT_STREQ(str, "\"\"");
  CPLFree(str);
}

TEST(SwqExprNodeTest, UnparseOperations) {
  // Binary infix ops
  swq_expr_node add_op(SWQ_ADD);
  add_op.PushSubExpression(new swq_expr_node(1));
  add_op.PushSubExpression(new swq_expr_node(2));
  char* str = add_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "1 + 2");
  CPLFree(str);

  // Nested operation adds parentheses
  swq_expr_node mul_op(SWQ_MULTIPLY);
  auto* nested = new swq_expr_node(SWQ_ADD);
  nested->PushSubExpression(new swq_expr_node(1));
  nested->PushSubExpression(new swq_expr_node(2));
  mul_op.PushSubExpression(nested);
  mul_op.PushSubExpression(new swq_expr_node(3));
  str = mul_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "(1 + 2) * 3");
  CPLFree(str);

  // LIKE without and with ESCAPE
  swq_expr_node like_op(SWQ_LIKE);
  like_op.PushSubExpression(new swq_expr_node("abc"));
  like_op.PushSubExpression(new swq_expr_node("a%"));
  str = like_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "'abc' LIKE 'a%'");
  CPLFree(str);

  swq_expr_node like_esc(SWQ_LIKE);
  like_esc.PushSubExpression(new swq_expr_node("abc_d"));
  like_esc.PushSubExpression(new swq_expr_node("abc\\_d"));
  like_esc.PushSubExpression(new swq_expr_node("\\"));
  str = like_esc.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "'abc_d' LIKE 'abc\\_d' ESCAPE '\\'");
  CPLFree(str);

  // NOT
  swq_expr_node not_op(SWQ_NOT);
  not_op.PushSubExpression(new swq_expr_node(1));
  str = not_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "NOT 1");
  CPLFree(str);

  // IS NULL
  swq_expr_node isnull_op(SWQ_ISNULL);
  isnull_op.PushSubExpression(new swq_expr_node("col"));
  str = isnull_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "'col' IS NULL");
  CPLFree(str);

  // IN
  swq_expr_node in_op(SWQ_IN);
  in_op.PushSubExpression(new swq_expr_node("x"));
  in_op.PushSubExpression(new swq_expr_node(1));
  in_op.PushSubExpression(new swq_expr_node(2));
  str = in_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "'x' IN(1,2)");
  CPLFree(str);

  // BETWEEN
  swq_expr_node btw_op(SWQ_BETWEEN);
  btw_op.PushSubExpression(new swq_expr_node("val"));
  btw_op.PushSubExpression(new swq_expr_node(10));
  btw_op.PushSubExpression(new swq_expr_node(20));
  str = btw_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "'val' BETWEEN 10 AND 20");
  CPLFree(str);

  // CAST 1, 2, and 3 arguments
  swq_expr_node cast_1(SWQ_CAST);
  cast_1.PushSubExpression(new swq_expr_node("foo"));
  str = cast_1.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "CAST('foo')");
  CPLFree(str);

  swq_expr_node cast_2(SWQ_CAST);
  cast_2.PushSubExpression(new swq_expr_node("foo"));
  cast_2.PushSubExpression(new swq_expr_node("INTEGER"));
  str = cast_2.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "CAST('foo' AS INTEGER)");
  CPLFree(str);

  swq_expr_node cast_3(SWQ_CAST);
  cast_3.PushSubExpression(new swq_expr_node("foo"));
  cast_3.PushSubExpression(new swq_expr_node("CHARACTER"));
  cast_3.PushSubExpression(new swq_expr_node(32));
  str = cast_3.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "CAST('foo' AS CHARACTER(32))");
  CPLFree(str);

  // Function style standard op and custom function
  swq_expr_node substr_op(SWQ_SUBSTR);
  substr_op.PushSubExpression(new swq_expr_node("abcdef"));
  substr_op.PushSubExpression(new swq_expr_node(1));
  substr_op.PushSubExpression(new swq_expr_node(3));
  str = substr_op.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "SUBSTR('abcdef',1,3)");
  CPLFree(str);

  swq_expr_node cust_fn(SWQ_CUSTOM_FUNC);
  cust_fn.string_value = CPLStrdup("MY_CUSTOM");
  cust_fn.PushSubExpression(new swq_expr_node(10));
  str = cust_fn.Unparse(nullptr, '"');
  EXPECT_STREQ(str, "MY_CUSTOM(10)");
  CPLFree(str);
}

TEST(SwqExprNodeTest, EvaluateMethod) {
  swq_evaluation_context sContext;

  // Constant evaluation returns clone
  swq_expr_node const_node(123);
  std::unique_ptr<swq_expr_node> res1(
      const_node.Evaluate(nullptr, nullptr, sContext));
  ASSERT_NE(res1, nullptr);
  EXPECT_EQ(res1->int_value, 123);

  // Column evaluation via fetcher
  auto fetcher = [](swq_expr_node* op, void* record) -> swq_expr_node* {
    if (op->string_value && strcmp(op->string_value, "age") == 0) {
      return new swq_expr_node(30);
    }
    return nullptr;
  };
  swq_expr_node col_node;
  col_node.eNodeType = SNT_COLUMN;
  col_node.string_value = CPLStrdup("age");
  std::unique_ptr<swq_expr_node> res2(
      col_node.Evaluate(fetcher, nullptr, sContext));
  ASSERT_NE(res2, nullptr);
  EXPECT_EQ(res2->int_value, 30);

  // Operation evaluation (SWQ_ADD 10 + 20)
  swq_expr_node add_op(SWQ_ADD);
  add_op.PushSubExpression(new swq_expr_node(10));
  add_op.PushSubExpression(new swq_expr_node(20));
  std::unique_ptr<swq_expr_node> res3(
      add_op.Evaluate(fetcher, nullptr, sContext));
  ASSERT_NE(res3, nullptr);
  EXPECT_EQ(res3->int_value, 30);

  // Operation evaluation with failing child
  {
    WithQuietHandler quiet_handler;
    swq_expr_node bad_add(SWQ_ADD);
    auto* missing_col = new swq_expr_node();
    missing_col->eNodeType = SNT_COLUMN;
    missing_col->string_value = CPLStrdup("nonexistent");
    bad_add.PushSubExpression(missing_col);
    bad_add.PushSubExpression(new swq_expr_node(20));
    EXPECT_EQ(bad_add.Evaluate(fetcher, nullptr, sContext), nullptr);
  }

  // Operation evaluation with unknown op code
  {
    WithQuietHandler quiet_handler;
    swq_expr_node invalid_op(SWQ_ARGUMENT_LIST);
    invalid_op.PushSubExpression(new swq_expr_node(1));
    EXPECT_EQ(invalid_op.Evaluate(fetcher, nullptr, sContext), nullptr);

    swq_expr_node invalid_custom(SWQ_CUSTOM_FUNC);
    invalid_custom.string_value = CPLStrdup("NONEXISTENT");
    invalid_custom.PushSubExpression(new swq_expr_node(1));
    EXPECT_EQ(invalid_custom.Evaluate(fetcher, nullptr, sContext), nullptr);
  }

  // Recursion depth limit (level 32)
  {
    WithQuietHandler quiet_handler;
    std::unique_ptr<swq_expr_node> top =
        std::make_unique<swq_expr_node>(SWQ_NOT);
    swq_expr_node* curr = top.get();
    for (int i = 0; i < 35; ++i) {
      auto* next = new swq_expr_node(SWQ_NOT);
      curr->PushSubExpression(next);
      curr = next;
    }
    curr->PushSubExpression(new swq_expr_node(1));
    EXPECT_EQ(top->Evaluate(fetcher, nullptr, sContext), nullptr);
  }
}

TEST(SwqExprNodeTest, ReplaceBetweenByGEAndLE) {
  // Non-operation node (no-op)
  swq_expr_node const_node(1);
  const_node.ReplaceBetweenByGEAndLERecurse();
  EXPECT_EQ(const_node.int_value, 1);

  // Non-BETWEEN operation (recurses)
  swq_expr_node add_node(SWQ_ADD);
  auto* btw_child = new swq_expr_node(SWQ_BETWEEN);
  btw_child->PushSubExpression(new swq_expr_node("x"));
  btw_child->PushSubExpression(new swq_expr_node(1));
  btw_child->PushSubExpression(new swq_expr_node(10));
  add_node.PushSubExpression(btw_child);
  add_node.ReplaceBetweenByGEAndLERecurse();
  EXPECT_EQ(add_node.papoSubExpr[0]->nOperation, SWQ_AND);

  // Valid BETWEEN: BETWEEN(x, 5, 15) -> AND(GE(x, 5), LE(x, 15))
  swq_expr_node btw(SWQ_BETWEEN);
  btw.PushSubExpression(new swq_expr_node("val"));
  btw.PushSubExpression(new swq_expr_node(5));
  btw.PushSubExpression(new swq_expr_node(15));
  btw.ReplaceBetweenByGEAndLERecurse();
  EXPECT_EQ(btw.nOperation, SWQ_AND);
  ASSERT_EQ(btw.nSubExprCount, 2);
  EXPECT_EQ(btw.papoSubExpr[0]->nOperation, SWQ_GE);
  EXPECT_EQ(btw.papoSubExpr[1]->nOperation, SWQ_LE);

  // Invalid BETWEEN (count != 3) is a no-op
  swq_expr_node bad_btw(SWQ_BETWEEN);
  bad_btw.PushSubExpression(new swq_expr_node(1));
  bad_btw.ReplaceBetweenByGEAndLERecurse();
  EXPECT_EQ(bad_btw.nOperation, SWQ_BETWEEN);
}

TEST(SwqExprNodeTest, ReplaceInByOrAndRebalance) {
  // Non-operation node (no-op)
  swq_expr_node const_node(1);
  const_node.ReplaceInByOrRecurse();
  EXPECT_EQ(const_node.int_value, 1);

  // IN with 3 target values: x IN (1, 2, 3) -> (x = 1 OR x = 2 OR x = 3)
  swq_expr_node in_node(SWQ_IN);
  in_node.PushSubExpression(new swq_expr_node("x"));
  in_node.PushSubExpression(new swq_expr_node(1));
  in_node.PushSubExpression(new swq_expr_node(2));
  in_node.PushSubExpression(new swq_expr_node(3));
  in_node.PushSubExpression(new swq_expr_node(4));
  in_node.PushSubExpression(new swq_expr_node(5));
  in_node.ReplaceInByOrRecurse();
  EXPECT_EQ(in_node.nOperation, SWQ_OR);
  EXPECT_EQ(in_node.nSubExprCount, 2);  // rebalanced to binary tree

  // RebalanceAndOr with 4 AND expressions
  swq_expr_node and_node(SWQ_AND);
  and_node.PushSubExpression(new swq_expr_node(1));
  and_node.PushSubExpression(new swq_expr_node(2));
  and_node.PushSubExpression(new swq_expr_node(3));
  and_node.PushSubExpression(new swq_expr_node(4));
  and_node.RebalanceAndOr();
  EXPECT_EQ(and_node.nOperation, SWQ_AND);
  EXPECT_EQ(and_node.nSubExprCount, 2);
}

TEST(SwqExprNodeTest, PushNotOperationDownToStack) {
  // Non-operation node (no-op)
  swq_expr_node const_node(1);
  const_node.PushNotOperationDownToStack();
  EXPECT_EQ(const_node.int_value, 1);

  // NOT(NOT A) -> A
  {
    swq_expr_node not1(SWQ_NOT);
    auto* not2 = new swq_expr_node(SWQ_NOT);
    not2->PushSubExpression(new swq_expr_node(42));
    not1.PushSubExpression(not2);
    not1.PushNotOperationDownToStack();
    EXPECT_EQ(not1.eNodeType, SNT_CONSTANT);
    EXPECT_EQ(not1.int_value, 42);
  }

  // NOT(A AND B) -> (NOT A) OR (NOT B)
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* and_op = new swq_expr_node(SWQ_AND);
    and_op->PushSubExpression(new swq_expr_node(1));
    and_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(and_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_OR);
    ASSERT_EQ(not_op.nSubExprCount, 2);
    EXPECT_EQ(not_op.papoSubExpr[0]->nOperation, SWQ_NOT);
    EXPECT_EQ(not_op.papoSubExpr[1]->nOperation, SWQ_NOT);
  }

  // NOT(A OR B) -> (NOT A) AND (NOT B)
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* or_op = new swq_expr_node(SWQ_OR);
    or_op->PushSubExpression(new swq_expr_node(1));
    or_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(or_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_AND);
    ASSERT_EQ(not_op.nSubExprCount, 2);
    EXPECT_EQ(not_op.papoSubExpr[0]->nOperation, SWQ_NOT);
    EXPECT_EQ(not_op.papoSubExpr[1]->nOperation, SWQ_NOT);
  }

  // Relational comparisons:
  // NOT(A == B) -> A != B
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* eq_op = new swq_expr_node(SWQ_EQ);
    eq_op->PushSubExpression(new swq_expr_node(1));
    eq_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(eq_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_NE);
  }

  // NOT(A != B) -> A == B
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* ne_op = new swq_expr_node(SWQ_NE);
    ne_op->PushSubExpression(new swq_expr_node(1));
    ne_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(ne_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_EQ);
  }

  // NOT(A > B) -> A <= B
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* gt_op = new swq_expr_node(SWQ_GT);
    gt_op->PushSubExpression(new swq_expr_node(1));
    gt_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(gt_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_LE);
  }

  // NOT(A >= B) -> A < B
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* ge_op = new swq_expr_node(SWQ_GE);
    ge_op->PushSubExpression(new swq_expr_node(1));
    ge_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(ge_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_LT);
  }

  // NOT(A < B) -> A >= B
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* lt_op = new swq_expr_node(SWQ_LT);
    lt_op->PushSubExpression(new swq_expr_node(1));
    lt_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(lt_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_GE);
  }

  // NOT(A <= B) -> A > B
  {
    swq_expr_node not_op(SWQ_NOT);
    auto* le_op = new swq_expr_node(SWQ_LE);
    le_op->PushSubExpression(new swq_expr_node(1));
    le_op->PushSubExpression(new swq_expr_node(2));
    not_op.PushSubExpression(le_op);
    not_op.PushNotOperationDownToStack();
    EXPECT_EQ(not_op.nOperation, SWQ_GT);
  }
}

}  // namespace
}  // namespace autotest2
