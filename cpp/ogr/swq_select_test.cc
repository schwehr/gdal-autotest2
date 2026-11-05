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

#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

swq_expr_node* MakeColumnNode(const char* field_name,
                              const char* table_name = nullptr) {
  auto* node = new swq_expr_node(field_name);
  node->eNodeType = SNT_COLUMN;
  if (table_name) {
    CPLFree(node->table_name);
    node->table_name = CPLStrdup(table_name);
  }
  return node;
}

struct FieldListFixture {
  swq_field_list field_list{};
  std::vector<char*> names;
  std::vector<swq_field_type> types;
  std::vector<int> ids;
  std::vector<int> table_ids;
  std::vector<swq_table_def> table_defs;

  void AddTable(const char* name, const char* alias) {
    swq_table_def def{};
    def.data_source = nullptr;
    def.table_name = CPLStrdup(name);
    def.table_alias = CPLStrdup(alias ? alias : name);
    table_defs.push_back(def);
  }

  void AddField(const char* name, swq_field_type type, int table_id = 0) {
    names.push_back(CPLStrdup(name));
    types.push_back(type);
    ids.push_back(static_cast<int>(names.size()) - 1);
    table_ids.push_back(table_id);
  }

  void Finalize() {
    field_list.count = static_cast<int>(names.size());
    field_list.names = names.data();
    field_list.types = types.data();
    field_list.ids = ids.data();
    field_list.table_ids = table_ids.data();
    field_list.table_count = static_cast<int>(table_defs.size());
    field_list.table_defs = table_defs.data();
  }

  ~FieldListFixture() {
    for (char* n : names) {
      CPLFree(n);
    }
    for (auto& td : table_defs) {
      CPLFree(td.data_source);
      CPLFree(td.table_name);
      CPLFree(td.table_alias);
    }
  }
};

TEST(SwqSelectTest, PreparseAndUnparseBasic) {
  swq_select sel;
  EXPECT_EQ(CE_None, sel.preparse("SELECT a, b AS my_b FROM tbl"));
  EXPECT_EQ(2, sel.result_columns());

  char* unparsed = sel.Unparse();
  ASSERT_NE(nullptr, unparsed);
  EXPECT_STREQ("SELECT a, b AS my_b FROM tbl", unparsed);
  CPLFree(unparsed);
}

TEST(SwqSelectTest, PreparseInvalidSql) {
  swq_select sel;
  WithQuietHandler quiet;
  EXPECT_EQ(CE_Failure, sel.preparse("NOT A VALID SELECT STATEMENT"));
}

TEST(SwqSelectTest, UnparseFullFeatures) {
  swq_select sel;
  const char* kSql =
      "SELECT DISTINCT a + 1 AS expr_alias, AVG(a), MIN(a), MAX(a), COUNT(a), "
      "COUNT(DISTINCT a), SUM(a), STDDEV_POP(a), STDDEV_SAMP(a) "
      "FROM 'datasource'.tbl AS t_alias "
      "JOIN 'ds2'.tbl2 AS t2 ON t_alias.id = t2.id "
      "JOIN tbl3 ON t_alias.id = tbl3.id "
      "WHERE a > 10 ORDER BY a ASC, b DESC LIMIT 5 OFFSET 2";

  EXPECT_EQ(CE_None, sel.preparse(kSql));
  EXPECT_EQ(5, sel.limit);
  EXPECT_EQ(2, sel.offset);

  char* unparsed = sel.Unparse();
  ASSERT_NE(nullptr, unparsed);
  EXPECT_THAT(unparsed, HasSubstr("SELECT DISTINCT "));
  EXPECT_THAT(unparsed, HasSubstr("AVG("));
  EXPECT_THAT(unparsed, HasSubstr("MIN("));
  EXPECT_THAT(unparsed, HasSubstr("MAX("));
  EXPECT_THAT(unparsed, HasSubstr("COUNT("));
  EXPECT_THAT(unparsed, HasSubstr("COUNT(DISTINCT "));
  EXPECT_THAT(unparsed, HasSubstr("SUM("));
  EXPECT_THAT(unparsed, HasSubstr("STDDEV_POP("));
  EXPECT_THAT(unparsed, HasSubstr("STDDEV_SAMP("));
  EXPECT_THAT(unparsed, HasSubstr("FROM 'datasource'.tbl AS t_alias"));
  EXPECT_THAT(unparsed, HasSubstr("JOIN 'ds2'.tbl2 AS t2 ON "));
  EXPECT_THAT(unparsed, HasSubstr("JOIN tbl3 ON "));
  EXPECT_THAT(unparsed, HasSubstr("WHERE "));
  EXPECT_THAT(unparsed, HasSubstr("ORDER BY a, b DESC"));
  EXPECT_THAT(unparsed, HasSubstr("LIMIT 5"));
  EXPECT_THAT(unparsed, HasSubstr("OFFSET 2"));
  CPLFree(unparsed);
}

TEST(SwqSelectTest, UnionAll) {
  swq_select sel1;
  EXPECT_EQ(CE_None,
            sel1.preparse("SELECT a FROM tbl UNION ALL SELECT b FROM tbl2"));
  ASSERT_NE(nullptr, sel1.poOtherSelect);
  EXPECT_EQ(1, sel1.poOtherSelect->result_columns());
}

TEST(SwqSelectTest, PushFieldDistinctErrors) {
  swq_select sel;
  sel.query_mode = SWQM_DISTINCT_LIST;

  WithQuietHandler quiet;
  auto* expr = MakeColumnNode("a");
  EXPECT_FALSE(sel.PushField(expr, nullptr, /*distinct_flag=*/true, false));
  delete expr;
}

TEST(SwqSelectTest, PushFieldHiddenKeyword) {
  swq_select sel;
  // Valid HIDDEN on OGR_STYLE
  auto* style_expr = MakeColumnNode("OGR_STYLE");
  EXPECT_TRUE(sel.PushField(style_expr, nullptr, false, /*bHidden=*/true));

  // Valid HIDDEN with alias OGR_STYLE
  auto* col_expr = MakeColumnNode("foo");
  EXPECT_TRUE(sel.PushField(col_expr, "OGR_STYLE", false, /*bHidden=*/true));

  // Invalid HIDDEN on other field
  WithQuietHandler quiet;
  auto* bad_expr = MakeColumnNode("other_field");
  EXPECT_FALSE(sel.PushField(bad_expr, nullptr, false, /*bHidden=*/true));
  delete bad_expr;
}

TEST(SwqSelectTest, PushFieldAutoAliases) {
  swq_select sel;

  // CONCAT auto-alias
  auto* concat_expr = new swq_expr_node(SWQ_CONCAT);
  concat_expr->PushSubExpression(MakeColumnNode("my_field"));
  concat_expr->PushSubExpression(new swq_expr_node("prefix_"));
  EXPECT_TRUE(sel.PushField(concat_expr, nullptr, false, false));
  EXPECT_STREQ("CONCAT_my_field", sel.column_defs.back().field_alias);

  // SUBSTR auto-alias
  auto* substr_expr = new swq_expr_node(SWQ_SUBSTR);
  substr_expr->PushSubExpression(MakeColumnNode("text_field"));
  substr_expr->PushSubExpression(new swq_expr_node(1));
  EXPECT_TRUE(sel.PushField(substr_expr, nullptr, false, false));
  EXPECT_STREQ("SUBSTR_text_field", sel.column_defs.back().field_alias);
}

TEST(SwqSelectTest, PushFieldCastAllTypes) {
  const char* kTypes[] = {"character", "boolean", "integer", "bigint",
                          "smallint",  "float",   "numeric", "timestamp",
                          "date",      "time",    "geometry"};

  for (const char* type_name : kTypes) {
    swq_select sel;
    auto* cast_expr = new swq_expr_node(SWQ_CAST);
    cast_expr->PushSubExpression(MakeColumnNode("field"));
    cast_expr->PushSubExpression(new swq_expr_node(type_name));
    EXPECT_TRUE(sel.PushField(cast_expr, nullptr, false, false))
        << "Failed for type " << type_name;
  }

  // Geometry with type identifier and SRID
  {
    swq_select sel;
    auto* cast_geom = new swq_expr_node(SWQ_CAST);
    cast_geom->PushSubExpression(MakeColumnNode("geom"));
    cast_geom->PushSubExpression(new swq_expr_node("geometry"));
    cast_geom->PushSubExpression(new swq_expr_node("POINT"));
    cast_geom->PushSubExpression(new swq_expr_node(4326));
    EXPECT_TRUE(sel.PushField(cast_geom, nullptr, false, false));
    EXPECT_EQ(wkbPoint, sel.column_defs.back().eGeomType);
    EXPECT_EQ(4326, sel.column_defs.back().nSRID);
  }

  // Numeric with precision=0 and width < 10 -> SWQ_INTEGER
  {
    swq_select sel;
    auto* cast_num = new swq_expr_node(SWQ_CAST);
    cast_num->PushSubExpression(MakeColumnNode("val"));
    cast_num->PushSubExpression(new swq_expr_node("numeric"));
    cast_num->PushSubExpression(new swq_expr_node(8));
    cast_num->PushSubExpression(new swq_expr_node(0));
    EXPECT_TRUE(sel.PushField(cast_num, nullptr, false, false));
    EXPECT_EQ(SWQ_INTEGER, sel.column_defs.back().target_type);
  }

  // Numeric with precision=0 and width 15 -> SWQ_INTEGER64
  {
    swq_select sel;
    auto* cast_num = new swq_expr_node(SWQ_CAST);
    cast_num->PushSubExpression(MakeColumnNode("val"));
    cast_num->PushSubExpression(new swq_expr_node("numeric"));
    cast_num->PushSubExpression(new swq_expr_node(15));
    cast_num->PushSubExpression(new swq_expr_node(0));
    EXPECT_TRUE(sel.PushField(cast_num, nullptr, false, false));
    EXPECT_EQ(SWQ_INTEGER64, sel.column_defs.back().target_type);
  }

  // CAST errors
  {
    WithQuietHandler quiet;

    // Unrecognized type
    swq_select sel;
    auto* bad_type = new swq_expr_node(SWQ_CAST);
    bad_type->PushSubExpression(MakeColumnNode("val"));
    bad_type->PushSubExpression(new swq_expr_node("invalid_type"));
    EXPECT_FALSE(sel.PushField(bad_type, nullptr, false, false));
    delete bad_type;

    // Geometry with non-string geom identifier
    auto* bad_geom = new swq_expr_node(SWQ_CAST);
    bad_geom->PushSubExpression(MakeColumnNode("geom"));
    bad_geom->PushSubExpression(new swq_expr_node("geometry"));
    bad_geom->PushSubExpression(new swq_expr_node(123));  // integer, not string
    EXPECT_FALSE(sel.PushField(bad_geom, nullptr, false, false));
    delete bad_geom;

    // Numeric with non-integer width
    auto* bad_width = new swq_expr_node(SWQ_CAST);
    bad_width->PushSubExpression(new swq_expr_node("val"));
    bad_width->PushSubExpression(new swq_expr_node("numeric"));
    bad_width->PushSubExpression(new swq_expr_node("not_int"));
    EXPECT_FALSE(sel.PushField(bad_width, nullptr, false, false));
    delete bad_width;
  }
}

TEST(SwqSelectTest, PushFieldAggregateErrors) {
  WithQuietHandler quiet;
  swq_select sel;

  // Aggregate with 0 subexpressions
  auto* agg_no_arg = new swq_expr_node(SWQ_SUM);
  EXPECT_FALSE(sel.PushField(agg_no_arg, nullptr, false, false));
  delete agg_no_arg;

  // Aggregate with non-column argument
  auto* agg_const = new swq_expr_node(SWQ_SUM);
  agg_const->PushSubExpression(new swq_expr_node(100));
  EXPECT_FALSE(sel.PushField(agg_const, nullptr, false, false));
  delete agg_const;
}

TEST(SwqSelectTest, PushExcludeFieldAndWildcardExcept) {
  // PushExcludeField with non-column
  {
    swq_select sel;
    auto* const_node = new swq_expr_node(42);
    EXPECT_FALSE(sel.PushExcludeField(const_node));
    delete const_node;
  }

  // Repeated field in EXCEPT
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_Failure, sel.preparse("SELECT * EXCEPT (a, a) FROM tbl"));
  }

  // Valid EXCEPT on normal and geometry field
  {
    FieldListFixture fixture;
    fixture.AddTable("tbl", "tbl");
    fixture.AddField("a", SWQ_INTEGER);
    fixture.AddField("b", SWQ_STRING);
    fixture.AddField("geom", SWQ_GEOMETRY);
    fixture.Finalize();

    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT * EXCEPT (a, geom) FROM tbl"));
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, nullptr));
    EXPECT_TRUE(sel.bExcludedGeometry);
    EXPECT_EQ(1, sel.result_columns());
    EXPECT_STREQ("b", sel.column_defs[0].field_name);
  }

  // EXCEPT with non-existent field fails in expand_wildcard
  {
    WithQuietHandler quiet;
    FieldListFixture fixture;
    fixture.AddTable("tbl", "tbl");
    fixture.AddField("a", SWQ_INTEGER);
    fixture.Finalize();

    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT * EXCEPT (nonexistent) FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }
}

TEST(SwqSelectTest, ExpandWildcardTablePrefixAndCompose) {
  FieldListFixture fixture;
  fixture.AddTable("tbl1", "t1");
  fixture.AddTable("tbl2", "t2");
  fixture.AddField("id", SWQ_INTEGER, 0);
  fixture.AddField("col_a", SWQ_STRING, 0);
  fixture.AddField("id", SWQ_INTEGER, 1);
  fixture.AddField("col_b", SWQ_STRING, 1);
  fixture.Finalize();

  // Expanding t1.*
  {
    swq_select sel;
    EXPECT_EQ(
        CE_None,
        sel.preparse("SELECT t1.* FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = t2.id"));
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, nullptr));
    EXPECT_EQ(2, sel.result_columns());
  }

  // Expanding unknown table.* fails
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT unknown_tbl.* FROM tbl1 t1"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Duplicate column name 'id' across tables causes compose=true
  {
    swq_select sel;
    EXPECT_EQ(
        CE_None,
        sel.preparse("SELECT * FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = t2.id"));
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, nullptr));
    EXPECT_EQ(4, sel.result_columns());
  }
}

TEST(SwqSelectTest, ParseColumnValidationErrors) {
  FieldListFixture fixture;
  fixture.AddTable("tbl", "tbl");
  fixture.AddField("num", SWQ_INTEGER);
  fixture.AddField("str", SWQ_STRING);
  fixture.AddField("geom", SWQ_GEOMETRY);
  fixture.Finalize();

  WithQuietHandler quiet;

  // Unrecognized column
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT bad_col FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Unrecognized column with table prefix
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT tbl.bad_col FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Invalid expression
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT num + bad_col FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Column function on geometry
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT SUM(geom) FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Illegal function on string
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT AVG(str) FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }
}

TEST(SwqSelectTest, ParseDistinctOptions) {
  FieldListFixture fixture;
  fixture.AddTable("tbl", "tbl");
  fixture.AddField("a", SWQ_INTEGER);
  fixture.AddField("b", SWQ_INTEGER);
  fixture.AddField("geom", SWQ_GEOMETRY);
  fixture.Finalize();

  // Distinct on multiple columns: default fails
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT DISTINCT a, b FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Distinct on multiple columns: with option succeeds
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT DISTINCT a, b FROM tbl"));
    swq_select_parse_options options;
    options.bAllowDistinctOnMultipleFields = TRUE;
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, &options));
  }

  // Distinct on geometry: default fails
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT DISTINCT geom FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Distinct on geometry: with option succeeds
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT DISTINCT geom FROM tbl"));
    swq_select_parse_options options;
    options.bAllowDistinctOnGeometryField = TRUE;
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, &options));
  }

  // COUNT DISTINCT on geometry fails
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT COUNT(DISTINCT geom) FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Mixed recordset and summary mode fails
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT a, COUNT(*) FROM tbl"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }
}

TEST(SwqSelectTest, ParseOrderByErrors) {
  FieldListFixture fixture;
  fixture.AddTable("tbl1", "t1");
  fixture.AddTable("tbl2", "t2");
  fixture.AddField("id", SWQ_INTEGER, 0);
  fixture.AddField("geom", SWQ_GEOMETRY, 0);
  fixture.AddField("sec_id", SWQ_INTEGER, 1);
  fixture.Finalize();

  WithQuietHandler quiet;

  // Unrecognized field in ORDER BY
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT id FROM tbl1 ORDER BY bad_field"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Unrecognized field with prefix in ORDER BY
  {
    swq_select sel;
    EXPECT_EQ(CE_None,
              sel.preparse("SELECT id FROM tbl1 ORDER BY t1.bad_field"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Field from secondary table in ORDER BY
  {
    swq_select sel;
    EXPECT_EQ(CE_None,
              sel.preparse("SELECT t1.id FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = "
                           "t2.sec_id ORDER BY t2.sec_id"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Geometry field in ORDER BY
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT id FROM tbl1 ORDER BY geom"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }
}

TEST(SwqSelectTest, ParseJoinCompatibilityChecks) {
  FieldListFixture fixture;
  fixture.AddTable("tbl1", "t1");
  fixture.AddTable("tbl2", "t2");
  fixture.AddTable("tbl3", "t3");
  fixture.AddField("id", SWQ_INTEGER, 0);
  fixture.AddField("t2_id", SWQ_INTEGER, 1);
  fixture.AddField("t3_id", SWQ_INTEGER, 2);
  fixture.Finalize();

  // Valid JOIN ON constant
  {
    swq_select sel;
    EXPECT_EQ(CE_None,
              sel.preparse("SELECT t1.id FROM tbl1 t1 JOIN tbl2 t2 ON 1 = 1"));
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, nullptr));
  }

  WithQuietHandler quiet;

  // Field in JOIN clause belonging to 3rd table (incompatible)
  {
    swq_select sel;
    EXPECT_EQ(CE_None,
              sel.preparse("SELECT t1.id FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = "
                           "t3.t3_id JOIN tbl3 t3 ON t1.id = t3.t3_id"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }
}

TEST(SwqSelectTest, ParseWhereClauseChecks) {
  FieldListFixture fixture;
  fixture.AddTable("tbl1", "t1");
  fixture.AddTable("tbl2", "t2");
  fixture.AddField("id", SWQ_INTEGER, 0);
  fixture.AddField("sec_val", SWQ_INTEGER, 1);
  fixture.Finalize();

  // Valid WHERE clause
  {
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT id FROM tbl1 WHERE id > 0"));
    EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, nullptr));
  }

  // Invalid WHERE clause (bad field)
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None, sel.preparse("SELECT id FROM tbl1 WHERE bad_col > 0"));
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
  }

  // Secondary table field in WHERE with and without option
  {
    WithQuietHandler quiet;
    swq_select sel;
    EXPECT_EQ(CE_None,
              sel.preparse("SELECT t1.id FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = "
                           "t2.sec_val WHERE t2.sec_val = 5"));
    // Default disallows secondary table in WHERE
    EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));

    // Option allows secondary table in WHERE
    swq_select sel2;
    EXPECT_EQ(CE_None,
              sel2.preparse("SELECT t1.id FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = "
                            "t2.sec_val WHERE t2.sec_val = 5"));
    swq_select_parse_options options;
    options.bAllowFieldsInSecondaryTablesInWhere = TRUE;
    EXPECT_EQ(CE_None, sel2.parse(&fixture.field_list, &options));
  }
}

TEST(SwqSelectTest, PushTableDefAndOrderByDirectCalls) {
  swq_select sel;
  EXPECT_EQ(0, sel.PushTableDef("ds1", "tbl1", "t1_alias"));
  EXPECT_EQ(1, sel.PushTableDef(nullptr, "tbl2", nullptr));
  EXPECT_EQ(2, sel.table_count);

  sel.PushOrderBy("tbl1", "col1", TRUE);
  sel.PushOrderBy(nullptr, "col2", FALSE);
  EXPECT_EQ(2, sel.order_specs);

  sel.SetLimit(100);
  sel.SetOffset(50);
  EXPECT_EQ(100, sel.limit);
  EXPECT_EQ(50, sel.offset);

  // PushJoin direct
  auto* join_expr = new swq_expr_node(1);
  sel.PushJoin(1, join_expr);
  EXPECT_EQ(1, sel.join_count);

  // PushUnionAll direct
  auto* other = new swq_select();
  sel.PushUnionAll(other);
  EXPECT_EQ(other, sel.poOtherSelect);
}

TEST(SwqSelectTest, UnparseEdgeCases) {
  swq_select sel;
  sel.PushTableDef("ds1", "tbl1", "t1_alias");
  sel.PushTableDef("ds2", "tbl2", "t2_alias");

  swq_col_def col{};
  col.field_name = CPLStrdup("my_col");
  col.table_name = CPLStrdup("tbl1");
  col.col_func = SWQCF_CUSTOM;
  sel.column_defs.push_back(col);

  char* unparsed = sel.Unparse();
  ASSERT_NE(nullptr, unparsed);
  EXPECT_THAT(unparsed, HasSubstr("tbl1.my_col"));
  CPLFree(unparsed);
}

TEST(SwqSelectTest, ParseZeroColumns) {
  swq_select sel;
  EXPECT_EQ(0, sel.result_columns());
  FieldListFixture fixture;
  fixture.AddTable("tbl", "tbl");
  fixture.Finalize();
  EXPECT_EQ(CE_None, sel.parse(&fixture.field_list, nullptr));
  EXPECT_EQ(SWQM_RECORDSET, sel.query_mode);
}

TEST(SwqSelectTest, CheckCompatibleJoinExprEdgeCases) {
  FieldListFixture fixture;
  fixture.AddTable("tbl1", "t1");
  fixture.AddTable("tbl2", "t2");
  fixture.AddField("id", SWQ_INTEGER, 0);
  fixture.AddField("t2_id", SWQ_INTEGER, 1);
  fixture.Finalize();

  WithQuietHandler quiet;
  swq_select sel;
  EXPECT_EQ(CE_None,
            sel.preparse(
                "SELECT t1.id FROM tbl1 t1 JOIN tbl2 t2 ON t1.id = t2.t2_id"));
  ASSERT_EQ(1, sel.join_count);

  // Modify join def expr to invalid column from table 3
  auto* col_node = MakeColumnNode("bad_field");
  col_node->table_index = 3;
  col_node->field_index = 0;
  delete sel.join_defs[0].poExpr;
  sel.join_defs[0].poExpr = col_node;

  EXPECT_EQ(CE_Failure, sel.parse(&fixture.field_list, nullptr));
}

}  // namespace
}  // namespace autotest2
