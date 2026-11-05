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
// Unit tests for swq_parser.cpp (OGR SQL Bison grammar parser).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/swq_parser.cpp

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

constexpr int SWQT_VALUE_START = 289;
constexpr int SWQT_SELECT_START = 290;

bool ParseValueExpr(const char* sql, bool accept_custom = false) {
  swq_parse_context context;
  context.nStartToken = SWQT_VALUE_START;
  context.pszInput = sql;
  context.pszNext = sql;
  context.bAcceptCustomFuncs = accept_custom ? TRUE : FALSE;
  int ret = swqparse(&context);
  delete context.poRoot;
  return ret == 0;
}

bool ParseSelect(const char* sql, bool accept_custom = false) {
  swq_select select;
  return select.preparse(sql, accept_custom ? TRUE : FALSE) == CE_None;
}

TEST(SWQParserTest, ValueExprComparisons) {
  EXPECT_TRUE(ParseValueExpr("a = 1"));
  EXPECT_TRUE(ParseValueExpr("a <> 1"));
  EXPECT_TRUE(ParseValueExpr("a != 1"));
  EXPECT_TRUE(ParseValueExpr("a < 1"));
  EXPECT_TRUE(ParseValueExpr("a > 1"));
  EXPECT_TRUE(ParseValueExpr("a <= 1"));
  EXPECT_TRUE(ParseValueExpr("a =< 1"));
  EXPECT_TRUE(ParseValueExpr("a => 1"));
  EXPECT_TRUE(ParseValueExpr("a >= 1"));
}

TEST(OGRSWQParserTest, ValueExprLikeAndIlike) {
  EXPECT_TRUE(ParseValueExpr("a LIKE 'pattern%'"));
  EXPECT_TRUE(ParseValueExpr("a NOT LIKE 'pattern%'"));
  EXPECT_TRUE(ParseValueExpr("a LIKE 'pattern%' ESCAPE '\\'"));
  EXPECT_TRUE(ParseValueExpr("a NOT LIKE 'pattern%' ESCAPE '\\'"));
  EXPECT_TRUE(ParseValueExpr("a ILIKE 'pattern%'"));
  EXPECT_TRUE(ParseValueExpr("a NOT ILIKE 'pattern%'"));
  EXPECT_TRUE(ParseValueExpr("a ILIKE 'pattern%' ESCAPE '\\'"));
  EXPECT_TRUE(ParseValueExpr("a NOT ILIKE 'pattern%' ESCAPE '\\'"));
}

TEST(SWQParserTest, ValueExprBetweenAndIn) {
  EXPECT_TRUE(ParseValueExpr("a BETWEEN 1 AND 10"));
  EXPECT_TRUE(ParseValueExpr("a NOT BETWEEN 1 AND 10"));
  EXPECT_TRUE(ParseValueExpr("a IN (1, 2, 3)"));
  EXPECT_TRUE(ParseValueExpr("a NOT IN ('x', 'y', 'z')"));
  EXPECT_TRUE(ParseValueExpr("a IS NULL"));
  EXPECT_TRUE(ParseValueExpr("a IS NOT NULL"));
}

TEST(SWQParserTest, ValueExprLogical) {
  EXPECT_TRUE(ParseValueExpr("a AND b"));
  EXPECT_TRUE(ParseValueExpr("a OR b"));
  EXPECT_TRUE(ParseValueExpr("NOT a"));
  EXPECT_TRUE(ParseValueExpr("(a AND b) OR (NOT c AND (d OR e))"));
  EXPECT_TRUE(ParseValueExpr("NOT (a OR b)"));
}

TEST(SWQParserTest, ValueExprArithmeticAndUnary) {
  EXPECT_TRUE(ParseValueExpr("a + b"));
  EXPECT_TRUE(ParseValueExpr("a - b"));
  EXPECT_TRUE(ParseValueExpr("a * b"));
  EXPECT_TRUE(ParseValueExpr("a / b"));
  EXPECT_TRUE(ParseValueExpr("a % b"));
  EXPECT_TRUE(ParseValueExpr("-a"));
  EXPECT_TRUE(ParseValueExpr("-5 + 10 * -(3 - 2)"));
  EXPECT_TRUE(ParseValueExpr("-(a + b)"));
}

TEST(SWQParserTest, ValueExprLiterals) {
  EXPECT_TRUE(ParseValueExpr("123"));
  EXPECT_TRUE(ParseValueExpr("-123"));
  EXPECT_TRUE(ParseValueExpr("123.456"));
  EXPECT_TRUE(ParseValueExpr("-123.456e-2"));
  EXPECT_TRUE(ParseValueExpr("'simple string'"));
  EXPECT_TRUE(ParseValueExpr("''"));
  EXPECT_TRUE(ParseValueExpr("TRUE"));
  EXPECT_TRUE(ParseValueExpr("FALSE"));
  EXPECT_TRUE(ParseValueExpr("NULL"));
}

TEST(SWQParserTest, ValueExprCast) {
  EXPECT_TRUE(ParseValueExpr("CAST(x AS character)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS character(32))"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS varchar)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS varchar(64))"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS integer)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS smallint)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS bigint)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS float)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS numeric)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS numeric(10))"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS numeric(10, 2))"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS date)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS time)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS timestamp)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS boolean)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS geometry)"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS geometry(point))"));
  EXPECT_TRUE(ParseValueExpr("CAST(x AS geometry(linestring, 4326))"));
  {
    WithQuietHandler holder;
    EXPECT_FALSE(ParseValueExpr("CAST(x AS GEOMETRY(invalid_geom))"));
    EXPECT_FALSE(ParseValueExpr("CAST(x AS NOT_GEOM(point))"));
    EXPECT_FALSE(ParseValueExpr("CAST(x AS GEOMETRY(invalid_geom, 4326))"));
    EXPECT_FALSE(ParseValueExpr("CAST(x AS NOT_GEOM(point, 4326))"));
  }
}

TEST(SWQParserTest, ValueExprFunctions) {
  EXPECT_TRUE(ParseValueExpr("my_func(a)", /*accept_custom=*/true));
  EXPECT_TRUE(
      ParseValueExpr("my_func(a, b, 123, 'str')", /*accept_custom=*/true));
  EXPECT_TRUE(ParseValueExpr("SUBSTR(col, 1, 3)"));
  EXPECT_TRUE(ParseValueExpr("HSTORE_GET_VALUE(tags, 'key')"));
  EXPECT_TRUE(ParseValueExpr("custom_func(1, 2)", /*accept_custom=*/true));
  {
    WithQuietHandler holder;
    // Functions must have at least one argument.
    EXPECT_FALSE(ParseValueExpr("my_func()", /*accept_custom=*/true));
    EXPECT_FALSE(
        ParseValueExpr("undefined_func(1, 2)", /*accept_custom=*/false));
  }
}

TEST(SWQParserTest, ValueExprUnaryMinus) {
  EXPECT_TRUE(ParseValueExpr("-1"));
  EXPECT_TRUE(ParseValueExpr("-1.5"));
  EXPECT_TRUE(ParseValueExpr("-a"));
  EXPECT_TRUE(ParseValueExpr("- 9223372036854775808"));
  EXPECT_TRUE(ParseValueExpr("- (-9223372036854775808)"));
}

TEST(SWQParserTest, ValueExprIdentifiers) {
  EXPECT_TRUE(ParseValueExpr("FID"));
  EXPECT_TRUE(ParseValueExpr("OGR_GEOMETRY"));
  EXPECT_TRUE(ParseValueExpr("OGR_STYLE"));
  EXPECT_TRUE(ParseValueExpr("OGR_GEOM_WKT"));
  EXPECT_TRUE(ParseValueExpr("OGR_GEOM_AREA"));
  EXPECT_TRUE(ParseValueExpr("tbl.col"));
  EXPECT_TRUE(ParseValueExpr("\"quoted col\""));
  EXPECT_TRUE(ParseValueExpr("\"table\".\"column\""));
}

TEST(SWQParserTest, SelectSimple) {
  EXPECT_TRUE(ParseSelect("SELECT col FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT col1, col2, col3 FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT tab.* FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT DISTINCT col FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT col AS alias FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT col alias FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT 1 + 2 AS calc FROM tab"));
}

TEST(SWQParserTest, SelectAggregates) {
  EXPECT_TRUE(ParseSelect("SELECT COUNT(*) FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT COUNT(*) AS total FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT COUNT(*) total FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT COUNT(col) FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT COUNT(DISTINCT col) FROM tab"));
  EXPECT_TRUE(
      ParseSelect("SELECT COUNT(DISTINCT col) AS distinct_count FROM tab"));
  EXPECT_TRUE(
      ParseSelect("SELECT COUNT(DISTINCT col) distinct_count FROM tab"));
  EXPECT_TRUE(
      ParseSelect("SELECT MIN(col), MAX(col), AVG(col), SUM(col) FROM tab"));
  {
    WithQuietHandler holder;
    EXPECT_FALSE(ParseSelect("SELECT FOO(*) FROM tab"));
    EXPECT_FALSE(ParseSelect("SELECT FOO(*) AS total FROM tab"));
    EXPECT_FALSE(ParseSelect("SELECT FOO(DISTINCT col) FROM tab"));
    EXPECT_FALSE(ParseSelect("SELECT FOO(DISTINCT col) AS total FROM tab"));
  }
}

TEST(SWQParserTest, SelectFromAndJoins) {
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab AS t"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab t"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM \"table name\""));
  EXPECT_TRUE(ParseSelect("SELECT * FROM 'datasource'.table"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM 'datasource'.table AS t"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM 'datasource'.\"table\""));
  EXPECT_TRUE(ParseSelect("SELECT * FROM ds.table"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM ds.table AS t"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab1 JOIN tab2 ON tab1.id = tab2.id"));
  EXPECT_TRUE(
      ParseSelect("SELECT * FROM tab1 LEFT JOIN tab2 ON tab1.id = tab2.id"));
  EXPECT_TRUE(
      ParseSelect("SELECT * FROM tab1 LEFT JOIN tab2 ON tab1.id = tab2.id LEFT "
                  "JOIN tab3 ON tab2.id = tab3.id"));
}

TEST(SWQParserTest, SelectWhere) {
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab WHERE id = 1"));
  EXPECT_TRUE(
      ParseSelect("SELECT * FROM tab WHERE name LIKE 'foo%' AND age > 20"));
  EXPECT_TRUE(ParseSelect(
      "SELECT * FROM tab WHERE (col1 IS NOT NULL) OR (col2 BETWEEN 1 AND 5)"));
}

TEST(SWQParserTest, SelectOrderBy) {
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab ORDER BY col"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab ORDER BY col ASC"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab ORDER BY col DESC"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab ORDER BY col1 ASC, col2 DESC"));
}

TEST(SWQParserTest, SelectLimitAndOffset) {
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab LIMIT 10"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab LIMIT 10 OFFSET 20"));
  EXPECT_TRUE(ParseSelect("SELECT * FROM tab OFFSET 20"));
}

TEST(SWQParserTest, SelectSpatial) {
  EXPECT_TRUE(ParseSelect("SELECT ST_Buffer(geom, 5) FROM tab",
                          /*accept_custom=*/true));
  EXPECT_TRUE(
      ParseSelect("SELECT * FROM tab WHERE ST_Intersects(geom, "
                  "ST_GeomFromText('POINT(0 0)'))",
                  /*accept_custom=*/true));
}

TEST(SWQParserTest, SelectUnion) {
  EXPECT_TRUE(ParseSelect("SELECT a FROM tab1 UNION ALL SELECT a FROM tab2"));
}

TEST(SWQParserTest, SelectExcludeAndHidden) {
  EXPECT_TRUE(ParseSelect("SELECT * EXCLUDE (col1) FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT * EXCEPT (col1, col2) FROM tab"));
  EXPECT_TRUE(ParseSelect("SELECT col AS OGR_STYLE HIDDEN FROM tab"));
  {
    WithQuietHandler holder;
    EXPECT_FALSE(ParseSelect("SELECT col AS my_col HIDDEN FROM tab"));
    EXPECT_FALSE(ParseSelect("SELECT * EXCLUDE (col1, col1) FROM tab"));
    EXPECT_FALSE(ParseSelect("SELECT COUNT(*) AS my_col HIDDEN FROM tab"));
    EXPECT_FALSE(
        ParseSelect("SELECT COUNT(DISTINCT col) AS my_col HIDDEN FROM tab"));
    EXPECT_FALSE(ParseSelect("SELECT CAST(x AS invalid_type) FROM tab"));
  }
}

TEST(SWQParserTest, SyntaxErrors) {
  WithQuietHandler holder;

  EXPECT_FALSE(ParseValueExpr(""));
  EXPECT_FALSE(ParseValueExpr("a ="));
  EXPECT_FALSE(ParseValueExpr("= a"));
  EXPECT_FALSE(ParseValueExpr("a AND"));
  EXPECT_FALSE(ParseValueExpr("a LIKE"));
  EXPECT_FALSE(ParseValueExpr("a BETWEEN"));
  EXPECT_FALSE(ParseValueExpr("a BETWEEN 1"));
  EXPECT_FALSE(ParseValueExpr("a BETWEEN 1 AND"));
  EXPECT_FALSE(ParseValueExpr("a IN (1,"));
  EXPECT_FALSE(ParseValueExpr("CAST(a AS)"));
  EXPECT_FALSE(ParseValueExpr("(a + b"));

  EXPECT_FALSE(ParseSelect(""));
  EXPECT_FALSE(ParseSelect("SELECT"));
  EXPECT_FALSE(ParseSelect("SELECT FROM tab"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab GROUP BY"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab ORDER BY"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab LIMIT"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab JOIN tab2"));
  EXPECT_FALSE(ParseSelect("SELECT col + FROM tab"));
  EXPECT_FALSE(ParseSelect("SELECT 1, 2, FROM tab"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE col = "));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE (col + )"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab JOIN tab2 ON"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab ORDER BY col, "));
  EXPECT_FALSE(ParseSelect("CAST(x AS geometry(point, 123 +))"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM 'my_str' +"));
  EXPECT_FALSE(ParseSelect("CAST(x AS identifier +)"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab ORDER BY col +"));
  EXPECT_FALSE(ParseSelect("CAST(x AS integer +)"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab AS alias +"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE col IN (1, 2, +)"));

  EXPECT_FALSE(ParseValueExpr(","));
  EXPECT_FALSE(ParseValueExpr("1,"));
  EXPECT_FALSE(ParseValueExpr("1,,2"));
  EXPECT_FALSE(ParseValueExpr("(,)"));
  EXPECT_FALSE(ParseSelect("SELECT , FROM tab"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM ,"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE ,"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE col = ,"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab ORDER BY ,"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab LIMIT ,"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab OFFSET ,"));
  EXPECT_FALSE(ParseSelect("SELECT col, , col2 FROM tab"));
  EXPECT_FALSE(ParseSelect("SELECT (,) FROM tab"));
  EXPECT_FALSE(ParseSelect("SELECT * FROM tab WHERE col IN (,)"));
}

TEST(SWQParserTest, MaxDepthExceeded) {
  WithQuietHandler holder;

  // 127 NOTs gives depth 127 for leaf expressions
  std::string deep127 = "1";
  for (int i = 0; i < 127; ++i) {
    deep127 = "NOT (" + deep127 + ")";
  }

  // 126 NOTs gives depth 126
  std::string deep126 = "1";
  for (int i = 0; i < 126; ++i) {
    deep126 = "NOT (" + deep126 + ")";
  }

  // Operators requiring depth 127
  EXPECT_FALSE(ParseValueExpr((deep127 + " = 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " <> 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " != 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " < 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " > 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " <= 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " =< 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " => 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " >= 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " LIKE 'a'").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " LIKE 'a' ESCAPE 'x'").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " ILIKE 'a'").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " ILIKE 'a' ESCAPE 'x'").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " IN (1)").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " BETWEEN 1 AND 2").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " IS NULL").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " + 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " - 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " * 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " / 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep127 + " % 1").c_str()));
  EXPECT_FALSE(ParseValueExpr(("1 IN (" + deep127 + ")").c_str()));
  EXPECT_FALSE(ParseValueExpr(("1 IN (1, " + deep127 + ")").c_str()));
  EXPECT_FALSE(ParseValueExpr(("1 IN (" + deep127 + ", 1)").c_str()));

  // Operators requiring depth 126
  EXPECT_FALSE(ParseValueExpr((deep126 + " AND 1").c_str()));
  EXPECT_FALSE(ParseValueExpr((deep126 + " OR 1").c_str()));
  EXPECT_FALSE(ParseValueExpr(("-(" + deep126 + ")").c_str()));
  EXPECT_FALSE(ParseValueExpr(("CAST(" + deep126 + " AS integer)").c_str()));
  EXPECT_FALSE(ParseValueExpr(("ABS(" + deep126 + ")").c_str()));

  // Sweep depths 120-130 for value_expr_list rules and custom funcs
  for (int d = 120; d <= 130; ++d) {
    std::string s = "1";
    for (int i = 0; i < d; ++i) s = "NOT (" + s + ")";
    ParseValueExpr(("1 IN (" + s + ", 2)").c_str());
    ParseValueExpr(("1 IN (2, " + s + ")").c_str());
    ParseValueExpr(("1 IN (2, 3, " + s + ")").c_str());
    ParseValueExpr(("1 IN (" + s + ", 2, 3)").c_str());
    ParseValueExpr(("custom_func(" + s + ", 2)").c_str(),
                   /*accept_custom=*/true);
    ParseValueExpr(("custom_func(2, " + s + ")").c_str(),
                   /*accept_custom=*/true);
    ParseValueExpr(("custom_func(" + s + ", 2, 3)").c_str(),
                   /*accept_custom=*/true);
    ParseValueExpr(("custom_func(2, " + s + ", 3)").c_str(),
                   /*accept_custom=*/true);
  }

  // Two-level composite NOT operators
  for (int d : {125, 126, 127}) {
    std::string s = "1";
    for (int i = 0; i < d; ++i) {
      s = "NOT (" + s + ")";
    }
    ParseValueExpr((s + " NOT LIKE 'a'").c_str());
    ParseValueExpr((s + " NOT LIKE 'a' ESCAPE 'x'").c_str());
    ParseValueExpr((s + " NOT ILIKE 'a'").c_str());
    ParseValueExpr((s + " NOT ILIKE 'a' ESCAPE 'x'").c_str());
    ParseValueExpr((s + " NOT IN (1)").c_str());
    ParseValueExpr((s + " NOT BETWEEN 1 AND 2").c_str());
  }
  EXPECT_FALSE(ParseValueExpr((deep126 + " IS NOT NULL").c_str()));
}

}  // namespace
}  // namespace autotest2
