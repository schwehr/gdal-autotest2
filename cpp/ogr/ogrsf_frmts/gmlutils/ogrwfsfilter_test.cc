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
// Tests for WFS filter translation (ogr/ogrsf_frmts/gmlutils/ogrwfsfilter.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gmlutils/ogrwfsfilter.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_wfs.py

#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/ogrwfsfilter.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class OgrWfsFilterTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(OgrWfsFilterTest, GmlObjectIdFilters) {
  // Test gml_id expression variants.
  swq_field_list field_list;
  const char* names[] = {"gml_id"};
  swq_field_type types[] = {SWQ_STRING};
  int ids[] = {0};
  int table_ids[] = {0};
  field_list.count = 1;
  field_list.names = const_cast<char**>(names);
  field_list.types = types;
  field_list.ids = ids;
  field_list.table_ids = table_ids;
  field_list.table_count = 0;
  field_list.table_defs = nullptr;

  auto CompileExpr = [&](const char* where) -> swq_expr_node* {
    swq_expr_node* root = nullptr;
    CPLErr err = swq_expr_compile2(where, &field_list, /*bCheck=*/1,
                                   WFSGetCustomFuncRegistrar(), &root);
    EXPECT_EQ(CE_None, err);
    return root;
  };

  int needs_null_check = 0;

  // 1. FeatureId (bUseFeatureId = TRUE).
  {
    swq_expr_node* expr = CompileExpr("gml_id = 'feat.1'");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, nullptr, 110,
        /*bPropertyIsNotEqualToSupported=*/1, /*bUseFeatureId=*/1,
        /*bGmlObjectIdNeedsGMLPrefix=*/0, "ogc:", &needs_null_check);
    EXPECT_EQ("<FeatureId fid=\"feat.1\"/>", filter);
  }

  // 2. ResourceId (nVersion >= 200).
  {
    swq_expr_node* expr = CompileExpr("gml_id = 'feat.2'");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, nullptr, 200,
        /*bPropertyIsNotEqualToSupported=*/1, /*bUseFeatureId=*/0,
        /*bGmlObjectIdNeedsGMLPrefix=*/0, "fes:", &needs_null_check);
    EXPECT_EQ("<ResourceId rid=\"feat.2\"/>", filter);
  }

  // 3. GmlObjectId without prefix (integer value).
  {
    auto expr = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("gml_id");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_STRING;
    expr->PushSubExpression(col);
    expr->PushSubExpression(new swq_expr_node(42));

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr.get(), nullptr, nullptr, 110,
        /*bPropertyIsNotEqualToSupported=*/1, /*bUseFeatureId=*/0,
        /*bGmlObjectIdNeedsGMLPrefix=*/0, "ogc:", &needs_null_check);
    EXPECT_EQ("<GmlObjectId id=\"42\"/>", filter);
  }

  // 4. GmlObjectId with gml: prefix and OR expression.
  {
    auto or_expr = std::make_unique<swq_expr_node>(SWQ_OR);
    auto eq1 = new swq_expr_node(SWQ_EQ);
    auto col1 = new swq_expr_node("gml_id");
    col1->eNodeType = SNT_COLUMN;
    col1->field_type = SWQ_STRING;
    eq1->PushSubExpression(col1);
    eq1->PushSubExpression(new swq_expr_node("a"));

    auto eq2 = new swq_expr_node(SWQ_EQ);
    auto col2 = new swq_expr_node("gml_id");
    col2->eNodeType = SNT_COLUMN;
    col2->field_type = SWQ_STRING;
    eq2->PushSubExpression(col2);
    eq2->PushSubExpression(new swq_expr_node(static_cast<GIntBig>(100)));

    or_expr->PushSubExpression(eq1);
    or_expr->PushSubExpression(eq2);

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        or_expr.get(), nullptr, nullptr, 110,
        /*bPropertyIsNotEqualToSupported=*/1, /*bUseFeatureId=*/0,
        /*bGmlObjectIdNeedsGMLPrefix=*/1, "ogc:", &needs_null_check);
    EXPECT_EQ("<GmlObjectId gml:id=\"a\"/><GmlObjectId gml:id=\"100\"/>",
              filter);
  }

  // 5. Float constant with gml_id (fails WFS_ExprDumpGmlObjectIdFilter).
  {
    auto expr = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("gml_id");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_STRING;
    expr->PushSubExpression(col);
    expr->PushSubExpression(new swq_expr_node(1.5));
    // Fails because gml_id is a special field not understood by server.
    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr.get(), nullptr, nullptr, 110,
        /*bPropertyIsNotEqualToSupported=*/1, /*bUseFeatureId=*/0,
        /*bGmlObjectIdNeedsGMLPrefix=*/0, "ogc:", &needs_null_check);
    EXPECT_TRUE(filter.empty());
  }
}

TEST_F(OgrWfsFilterTest, ComparisonAndLogicalFilters) {
  OGRFeatureDefn fdefn("test_layer");
  OGRFieldDefn f_int("age", OFTInteger);
  OGRFieldDefn f_real("score", OFTReal);
  OGRFieldDefn f_str("name", OFTString);
  fdefn.AddFieldDefn(&f_int);
  fdefn.AddFieldDefn(&f_real);
  fdefn.AddFieldDefn(&f_str);

  swq_field_list field_list;
  const char* names[] = {"age", "score", "name", "FID"};
  swq_field_type types[] = {SWQ_INTEGER, SWQ_FLOAT, SWQ_STRING, SWQ_INTEGER};
  int ids[] = {0, 1, 2, 3};
  int table_ids[] = {0, 0, 0, 0};
  field_list.count = 4;
  field_list.names = const_cast<char**>(names);
  field_list.types = types;
  field_list.ids = ids;
  field_list.table_ids = table_ids;
  field_list.table_count = 0;
  field_list.table_defs = nullptr;

  auto CompileExpr = [&](const char* where) -> swq_expr_node* {
    swq_expr_node* root = nullptr;
    CPLErr err = swq_expr_compile2(where, &field_list, /*bCheck=*/1,
                                   WFSGetCustomFuncRegistrar(), &root);
    EXPECT_EQ(CE_None, err);
    return root;
  };

  int needs_null_check = 0;

  // 1. Equal and comparison (<, <=, >, >=).
  {
    swq_expr_node* expr = CompileExpr(
        "age = 25 AND score < 100.5 AND age >= 18 AND score <= 99.0 AND age > "
        "10");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 110,
        /*bPropertyIsNotEqualToSupported=*/1, /*bUseFeatureId=*/0,
        /*bGmlObjectIdNeedsGMLPrefix=*/0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("<ogc:PropertyIsEqualTo>"));
    EXPECT_THAT(filter, HasSubstr("<ogc:PropertyIsLessThan>"));
    EXPECT_THAT(filter, HasSubstr("<ogc:PropertyIsGreaterThanOrEqualTo>"));
    EXPECT_THAT(filter, HasSubstr("<ogc:PropertyIsLessThanOrEqualTo>"));
    EXPECT_THAT(filter, HasSubstr("<ogc:PropertyIsGreaterThan>"));
  }

  // 2. NotEqual with bPropertyIsNotEqualToSupported = 1 vs 0.
  {
    swq_expr_node* expr1 = CompileExpr("age <> 30");
    ASSERT_NE(nullptr, expr1);
    absl::Cleanup cleaner1 = [expr1] { delete expr1; };

    CPLString filter1 = WFS_TurnSQLFilterToOGCFilter(
        expr1, nullptr, &fdefn, 110,
        /*bPropertyIsNotEqualToSupported=*/1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter1, HasSubstr("<ogc:PropertyIsNotEqualTo>"));

    swq_expr_node* expr2 = CompileExpr("age <> 30");
    ASSERT_NE(nullptr, expr2);
    absl::Cleanup cleaner2 = [expr2] { delete expr2; };

    CPLString filter2 = WFS_TurnSQLFilterToOGCFilter(
        expr2, nullptr, &fdefn, 110,
        /*bPropertyIsNotEqualToSupported=*/0, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter2, HasSubstr("<ogc:Not><ogc:PropertyIsEqualTo>"));
  }

  // 3. IS NULL and NOT.
  {
    swq_expr_node* expr = CompileExpr("NOT (name IS NULL)");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("<ogc:Not><ogc:PropertyIsNull>"));
    EXPECT_EQ(1, needs_null_check);
  }

  // 4. LIKE and ILIKE (version 100 vs version 200).
  {
    swq_expr_node* expr = CompileExpr("name LIKE 'A%B_!*'");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    // Version 100 uses escape="!" and converts % -> * and !* -> !!*
    CPLString filter100 = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 100, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter100, HasSubstr("escape=\"!\""));
    EXPECT_THAT(filter100, HasSubstr("<ogc:Literal>A*B_!!!*</ogc:Literal>"));
    EXPECT_THAT(filter100, HasSubstr("matchCase=\"true\""));
    EXPECT_THAT(filter100,
                HasSubstr("<ogc:PropertyName>name</ogc:PropertyName>"));

    // Version 200 uses ValueReference and escapeChar="!"
    swq_expr_node* expr2 = CompileExpr("name ILIKE 'A%B'");
    ASSERT_NE(nullptr, expr2);
    absl::Cleanup cleaner2 = [expr2] { delete expr2; };

    CPLString filter200 = WFS_TurnSQLFilterToOGCFilter(
        expr2, nullptr, &fdefn, 200, 1, 0, 0, "fes:", &needs_null_check);
    EXPECT_THAT(filter200, HasSubstr("escapeChar=\"!\""));
    EXPECT_THAT(filter200, HasSubstr("matchCase=\"false\""));
    EXPECT_THAT(filter200,
                HasSubstr("<fes:ValueReference>name</fes:ValueReference>"));
  }

  // 5. Special field FID rejected.
  {
    swq_expr_node* expr = CompileExpr("FID = 10");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_TRUE(filter.empty());
  }

  // 6. Timestamp literal (valid and invalid).
  {
    auto eq = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("score");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_FLOAT;
    eq->PushSubExpression(col);
    auto ts = new swq_expr_node("2020-01-02 12:34:56");
    ts->MarkAsTimestamp();
    eq->PushSubExpression(ts);

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        eq.get(), nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("2020-01-02T12:34:56"));
  }
  {
    auto eq = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("score");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_FLOAT;
    eq->PushSubExpression(col);
    auto ts = new swq_expr_node("invalid_date");
    ts->MarkAsTimestamp();
    eq->PushSubExpression(ts);

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        eq.get(), nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_TRUE(filter.empty());
  }

  // 7. No FDefn and no Dataset (field passed as-is).
  {
    swq_expr_node* expr = CompileExpr("age = 10");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, nullptr, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("<ogc:PropertyName>age</ogc:PropertyName>"));
  }

  // 8. Unknown field in FDefn.
  {
    auto eq = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("unknown_field");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_INTEGER;
    eq->PushSubExpression(col);
    eq->PushSubExpression(new swq_expr_node(10));

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        eq.get(), nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_TRUE(filter.empty());
  }

  // 9. Unknown field with table_name.
  {
    auto eq = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("unknown_field");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_INTEGER;
    col->table_name = CPLStrdup("other_table");
    eq->PushSubExpression(col);
    eq->PushSubExpression(new swq_expr_node(10));

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        eq.get(), nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_TRUE(filter.empty());
  }
}

TEST_F(OgrWfsFilterTest, DatasetTableLookup) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  auto srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  OGRLayer* layer = ds->CreateLayer("layer1", srs);
  srs->Release();
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_val("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_val));

  int needs_null_check = 0;

  // 1. Column in layer of poDS.
  {
    auto eq = std::make_unique<swq_expr_node>(SWQ_EQ);
    auto col = new swq_expr_node("val");
    col->eNodeType = SNT_COLUMN;
    col->field_type = SWQ_INTEGER;
    col->table_name = CPLStrdup("layer1");
    eq->PushSubExpression(col);
    eq->PushSubExpression(new swq_expr_node(42));

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        eq.get(), ds.get(), nullptr, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter,
                HasSubstr("<ogc:PropertyName>layer1/val</ogc:PropertyName>"));
  }

  // 2. Geometry column in layer of poDS with spatial function.
  {
    auto geom_col = new swq_expr_node("");
    geom_col->eNodeType = SNT_COLUMN;
    geom_col->field_type = SWQ_GEOMETRY;
    geom_col->table_name = CPLStrdup("layer1");

    auto op_geom = new swq_expr_node(SWQ_CUSTOM_FUNC);
    op_geom->string_value = CPLStrdup("ST_GeomFromText");
    op_geom->PushSubExpression(new swq_expr_node("POINT (10 20)"));

    auto st_intersects = std::make_unique<swq_expr_node>(SWQ_CUSTOM_FUNC);
    st_intersects->string_value = CPLStrdup("ST_Intersects");
    st_intersects->PushSubExpression(geom_col);
    st_intersects->PushSubExpression(op_geom);

    CPLString filter =
        WFS_TurnSQLFilterToOGCFilter(st_intersects.get(), ds.get(), nullptr,
                                     110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("<ogc:Intersects>"));
    EXPECT_THAT(filter, HasSubstr("srsName=\"urn:ogc:def:crs:EPSG::4326\""));
  }
}

TEST_F(OgrWfsFilterTest, SpatialFunctions) {
  OGRFeatureDefn fdefn("roads");
  OGRGeomFieldDefn gdefn("geom", wkbPoint);
  auto srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  gdefn.SetSpatialRef(srs);
  srs->Release();
  fdefn.AddGeomFieldDefn(&gdefn);

  swq_field_list field_list;
  const char* names[] = {"geom"};
  swq_field_type types[] = {SWQ_GEOMETRY};
  int ids[] = {0};
  int table_ids[] = {0};
  field_list.count = 1;
  field_list.names = const_cast<char**>(names);
  field_list.types = types;
  field_list.ids = ids;
  field_list.table_ids = table_ids;
  field_list.table_count = 0;
  field_list.table_defs = nullptr;

  auto CompileExpr = [&](const char* where) -> swq_expr_node* {
    swq_expr_node* root = nullptr;
    CPLErr err = swq_expr_compile2(where, &field_list, /*bCheck=*/1,
                                   WFSGetCustomFuncRegistrar(), &root);
    EXPECT_EQ(CE_None, err);
    return root;
  };

  int needs_null_check = 0;

  // 1. ST_Intersects with ST_MakeEnvelope (5 args with SRID, triggering axis
  // swap).
  {
    swq_expr_node* expr = CompileExpr(
        "ST_Intersects(geom, ST_MakeEnvelope(1.0, 2.0, 3.0, 4.0, 4326))");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("<ogc:Intersects>"));
    EXPECT_THAT(
        filter,
        HasSubstr("<gml:Envelope srsName=\"urn:ogc:def:crs:EPSG::4326\">"));
    EXPECT_THAT(filter, HasSubstr("<gml:lowerCorner>2 1</gml:lowerCorner>"));
    EXPECT_THAT(filter, HasSubstr("<gml:upperCorner>4 3</gml:upperCorner>"));
  }

  // 2. ST_DWithin with ST_GeomFromText and distance.
  {
    swq_expr_node* expr = CompileExpr(
        "ST_DWithin(geom, ST_GeomFromText('POINT (10 20)', 'EPSG:4326'), 50)");
    ASSERT_NE(nullptr, expr);
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_THAT(filter, HasSubstr("<ogc:DWithin>"));
    EXPECT_THAT(filter,
                HasSubstr("<ogc:Distance unit=\"m\">50</ogc:Distance>"));
    EXPECT_THAT(filter, HasSubstr("<gml:Point"));
  }

  // 3. Other spatial predicates: ST_Equals, ST_Disjoint, ST_Touches,
  // ST_Contains, ST_Within, ST_Crosses, ST_Overlaps, ST_Beyond.
  const char* preds[] = {
      "ST_Equals(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Disjoint(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Touches(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Contains(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Within(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Crosses(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Overlaps(geom, ST_GeomFromText('POINT (1 1)'))",
      "ST_Beyond(geom, ST_GeomFromText('POINT (1 1)'), 10)",
  };

  for (const char* p : preds) {
    swq_expr_node* expr = CompileExpr(p);
    ASSERT_NE(nullptr, expr) << p;
    absl::Cleanup cleaner = [expr] { delete expr; };

    CPLString filter = WFS_TurnSQLFilterToOGCFilter(
        expr, nullptr, &fdefn, 110, 1, 0, 0, "ogc:", &needs_null_check);
    EXPECT_FALSE(filter.empty()) << p;
  }
}

TEST_F(OgrWfsFilterTest, CustomFuncRegistrarAndCheckers) {
  swq_custom_func_registrar* registrar = WFSGetCustomFuncRegistrar();
  ASSERT_NE(nullptr, registrar);

  EXPECT_EQ(nullptr, registrar->GetOperator("NonExistentOp"));
  EXPECT_NE(nullptr, registrar->GetOperator("ST_Intersects"));
  EXPECT_NE(nullptr, registrar->GetOperator("ST_MakeEnvelope"));
  EXPECT_NE(nullptr, registrar->GetOperator("ST_GeomFromText"));
  EXPECT_NE(nullptr, registrar->GetOperator("ST_DWithin"));

  WithQuietHandler quiet;

  // Test ST_Intersects checker errors.
  const swq_operation* op_intersects = registrar->GetOperator("ST_Intersects");
  ASSERT_NE(nullptr, op_intersects);

  swq_expr_node node_bad_args(SWQ_CUSTOM_FUNC);
  node_bad_args.string_value = CPLStrdup("ST_Intersects");
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_intersects->pfnChecker(&node_bad_args, 0));

  swq_expr_node node_bad_type(SWQ_CUSTOM_FUNC);
  node_bad_type.string_value = CPLStrdup("ST_Intersects");
  node_bad_type.PushSubExpression(new swq_expr_node(10));
  node_bad_type.PushSubExpression(new swq_expr_node(20));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_intersects->pfnChecker(&node_bad_type, 0));

  // Test ST_MakeEnvelope checker errors.
  const swq_operation* op_envelope = registrar->GetOperator("ST_MakeEnvelope");
  ASSERT_NE(nullptr, op_envelope);

  swq_expr_node node_env_bad(SWQ_CUSTOM_FUNC);
  node_env_bad.string_value = CPLStrdup("ST_MakeEnvelope");
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_envelope->pfnChecker(&node_env_bad, 0));

  swq_expr_node node_env_bad_field(SWQ_CUSTOM_FUNC);
  node_env_bad_field.string_value = CPLStrdup("ST_MakeEnvelope");
  node_env_bad_field.PushSubExpression(new swq_expr_node("bad_str"));
  node_env_bad_field.PushSubExpression(new swq_expr_node(1.0));
  node_env_bad_field.PushSubExpression(new swq_expr_node(2.0));
  node_env_bad_field.PushSubExpression(new swq_expr_node(3.0));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_envelope->pfnChecker(&node_env_bad_field, 0));

  // Test ST_MakeEnvelope checker errors with invalid SRIDs.
  swq_expr_node node_env_bad_srid_int(SWQ_CUSTOM_FUNC);
  node_env_bad_srid_int.string_value = CPLStrdup("ST_MakeEnvelope");
  node_env_bad_srid_int.PushSubExpression(new swq_expr_node(1.0));
  node_env_bad_srid_int.PushSubExpression(new swq_expr_node(2.0));
  node_env_bad_srid_int.PushSubExpression(new swq_expr_node(3.0));
  node_env_bad_srid_int.PushSubExpression(new swq_expr_node(4.0));
  node_env_bad_srid_int.PushSubExpression(new swq_expr_node(-999999));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_envelope->pfnChecker(&node_env_bad_srid_int, 0));

  swq_expr_node node_env_bad_srid_str(SWQ_CUSTOM_FUNC);
  node_env_bad_srid_str.string_value = CPLStrdup("ST_MakeEnvelope");
  node_env_bad_srid_str.PushSubExpression(new swq_expr_node(1.0));
  node_env_bad_srid_str.PushSubExpression(new swq_expr_node(2.0));
  node_env_bad_srid_str.PushSubExpression(new swq_expr_node(3.0));
  node_env_bad_srid_str.PushSubExpression(new swq_expr_node(4.0));
  node_env_bad_srid_str.PushSubExpression(new swq_expr_node("INVALID_SRS"));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_envelope->pfnChecker(&node_env_bad_srid_str, 0));

  swq_expr_node node_env_bad_srid_float(SWQ_CUSTOM_FUNC);
  node_env_bad_srid_float.string_value = CPLStrdup("ST_MakeEnvelope");
  node_env_bad_srid_float.PushSubExpression(new swq_expr_node(1.0));
  node_env_bad_srid_float.PushSubExpression(new swq_expr_node(2.0));
  node_env_bad_srid_float.PushSubExpression(new swq_expr_node(3.0));
  node_env_bad_srid_float.PushSubExpression(new swq_expr_node(4.0));
  node_env_bad_srid_float.PushSubExpression(new swq_expr_node(4326.5));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_envelope->pfnChecker(&node_env_bad_srid_float, 0));

  // Test ST_GeomFromText checker errors.
  const swq_operation* op_geom = registrar->GetOperator("ST_GeomFromText");
  ASSERT_NE(nullptr, op_geom);

  swq_expr_node node_geom_bad_cnt(SWQ_CUSTOM_FUNC);
  node_geom_bad_cnt.string_value = CPLStrdup("ST_GeomFromText");
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_geom->pfnChecker(&node_geom_bad_cnt, 0));

  swq_expr_node node_geom_bad_type(SWQ_CUSTOM_FUNC);
  node_geom_bad_type.string_value = CPLStrdup("ST_GeomFromText");
  node_geom_bad_type.PushSubExpression(new swq_expr_node(100));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_geom->pfnChecker(&node_geom_bad_type, 0));

  swq_expr_node node_geom_bad_wkt(SWQ_CUSTOM_FUNC);
  node_geom_bad_wkt.string_value = CPLStrdup("ST_GeomFromText");
  node_geom_bad_wkt.PushSubExpression(new swq_expr_node("INVALID WKT"));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_geom->pfnChecker(&node_geom_bad_wkt, 0));

  swq_expr_node node_geom_bad_srid(SWQ_CUSTOM_FUNC);
  node_geom_bad_srid.string_value = CPLStrdup("ST_GeomFromText");
  node_geom_bad_srid.PushSubExpression(new swq_expr_node("POINT (0 0)"));
  node_geom_bad_srid.PushSubExpression(new swq_expr_node("INVALID_SRS"));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_geom->pfnChecker(&node_geom_bad_srid, 0));

  // Test ST_DWithin checker errors.
  const swq_operation* op_dwithin = registrar->GetOperator("ST_DWithin");
  ASSERT_NE(nullptr, op_dwithin);

  swq_expr_node node_dw_bad_cnt(SWQ_CUSTOM_FUNC);
  node_dw_bad_cnt.string_value = CPLStrdup("ST_DWithin");
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_dwithin->pfnChecker(&node_dw_bad_cnt, 0));

  swq_expr_node node_dw_bad_dist(SWQ_CUSTOM_FUNC);
  node_dw_bad_dist.string_value = CPLStrdup("ST_DWithin");
  OGRPoint pt1(0, 0);
  OGRPoint pt2(1, 1);
  auto pt_geom1 = new swq_expr_node(&pt1);
  auto pt_geom2 = new swq_expr_node(&pt2);
  node_dw_bad_dist.PushSubExpression(pt_geom1);
  node_dw_bad_dist.PushSubExpression(pt_geom2);
  node_dw_bad_dist.PushSubExpression(new swq_expr_node("not_a_number"));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_dwithin->pfnChecker(&node_dw_bad_dist, 0));
  swq_expr_node node_dw_bad_geom(SWQ_CUSTOM_FUNC);
  node_dw_bad_geom.string_value = CPLStrdup("ST_DWithin");
  node_dw_bad_geom.PushSubExpression(new swq_expr_node(10));
  node_dw_bad_geom.PushSubExpression(new swq_expr_node(20));
  node_dw_bad_geom.PushSubExpression(new swq_expr_node(5.0));
  CPLErrorReset();
  EXPECT_EQ(SWQ_ERROR, op_dwithin->pfnChecker(&node_dw_bad_geom, 0));
  CPLErrorReset();
}

// TODO: b/335317901 - OGRWfsFilter achieves 93.19% line coverage. Track
// OGR_SQL_LIKE_AS_ILIKE, ST_GeomFromText with integer SRID, and
// nUniqueGeomGMLId increments.

}  // namespace
}  // namespace autotest2
