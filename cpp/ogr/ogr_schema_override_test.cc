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

#include "third_party/gdal/ogr/ogr_schema_override.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"

namespace autotest2 {
namespace {

TEST(OgrSchemaOverrideTest, DefnOverrideGettersSetters) {
  OGRFieldDefnOverride fld;
  EXPECT_FALSE(fld.IsValid());

  fld.SetFieldName("new_name");
  EXPECT_TRUE(fld.IsValid());
  EXPECT_EQ("new_name", fld.GetFieldName().value());

  fld.SetFieldType(OFTInteger);
  EXPECT_EQ(OFTInteger, fld.GetFieldType().value());

  fld.SetFieldSubType(OFSTBoolean);
  EXPECT_EQ(OFSTBoolean, fld.GetFieldSubType().value());

  fld.SetSrcFieldType(OFTString);
  EXPECT_EQ(OFTString, fld.GetSrcFieldType().value());

  fld.SetSrcFieldSubType(OFSTNone);
  EXPECT_EQ(OFSTNone, fld.GetSrcFieldSubType().value());

  fld.SetFieldWidth(10);
  EXPECT_EQ(10, fld.GetFieldWidth().value());

  fld.SetFieldPrecision(2);
  EXPECT_EQ(2, fld.GetFieldPrecision().value());
}

TEST(OgrSchemaOverrideTest, LayerSchemaOverride) {
  OGRLayerSchemaOverride layer;
  EXPECT_FALSE(layer.IsValid());

  layer.SetLayerName("test_layer");
  EXPECT_FALSE(layer.IsValid());  // still no fields

  OGRFieldDefnOverride fld;
  fld.SetFieldType(OFTReal);
  layer.AddNamedFieldOverride("field1", fld);
  EXPECT_TRUE(layer.IsValid());
  EXPECT_EQ("test_layer", layer.GetLayerName());
  EXPECT_FALSE(layer.IsFullOverride());

  layer.SetFullOverride(true);
  EXPECT_TRUE(layer.IsFullOverride());

  OGRFieldDefnOverride unnamed_fld;
  unnamed_fld.SetFieldType(OFTInteger64);
  layer.AddUnnamedFieldOverride(unnamed_fld);
  EXPECT_EQ(1, layer.GetUnnamedFieldOverrides().size());
}

TEST(OgrSchemaOverrideTest, LoadFromJSONValidationErrors) {
  WithQuietHandler quiet_handler;
  OGRSchemaOverride schema;

  // Invalid JSON
  EXPECT_FALSE(schema.LoadFromJSON("{ invalid json"));

  // Non-object root: loads document but has no valid layers
  EXPECT_TRUE(schema.LoadFromJSON("[1, 2]"));
  EXPECT_FALSE(schema.IsValid());

  // Invalid layer entry
  EXPECT_FALSE(schema.LoadFromJSON("{\"layers\": [123]}"));

  // Layer without valid overrides
  EXPECT_FALSE(schema.LoadFromJSON("{\"layers\": [{\"name\": \"l1\"}]}"));

  // srcType in Full override mode
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", \"schemaType\": \"Full\", "
      "\"fields\": [{\"srcType\": \"integer\"}]}]}"));

  // srcType combined with name
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", "
      "\"fields\": [{\"name\": \"f1\", \"srcType\": \"integer\"}]}]}"));

  // Unsupported srcType
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", "
      "\"fields\": [{\"srcType\": \"unsupported_type\"}]}]}"));

  // srcSubType in Full override mode
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", \"schemaType\": \"Full\", "
      "\"fields\": [{\"srcSubType\": \"boolean\", \"subType\": "
      "\"boolean\"}]}]}"));

  // srcSubType combined with name
  EXPECT_FALSE(
      schema.LoadFromJSON("{\"layers\": [{\"name\": \"l1\", "
                          "\"fields\": [{\"name\": \"f1\", \"srcSubType\": "
                          "\"boolean\", \"subType\": \"boolean\"}]}]}"));

  // Unsupported srcSubType
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", "
      "\"fields\": [{\"srcSubType\": \"unsupported_subtype\"}]}]}"));

  // Missing field name and src types
  EXPECT_FALSE(
      schema.LoadFromJSON("{\"layers\": [{\"name\": \"l1\", "
                          "\"fields\": [{}]}]}"));

  // Unsupported target field type
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", "
      "\"fields\": [{\"name\": \"f1\", \"type\": \"unsupported_type\"}]}]}"));

  // Unsupported target field subtype
  EXPECT_FALSE(
      schema.LoadFromJSON("{\"layers\": [{\"name\": \"l1\", "
                          "\"fields\": [{\"name\": \"f1\", \"subType\": "
                          "\"unsupported_subtype\"}]}]}"));

  // Field with no valid overrides in Patch mode
  EXPECT_FALSE(schema.LoadFromJSON(
      "{\"layers\": [{\"name\": \"l1\", \"schemaType\": \"Patch\", "
      "\"fields\": [{\"name\": \"f1\"}]}]}"));
}

TEST(OgrSchemaOverrideTest, LoadFromJSONAndFile) {
  const std::string kValidJson =
      "{\n"
      "  \"layers\": [\n"
      "    {\n"
      "      \"name\": \"layer1\",\n"
      "      \"schemaType\": \"Patch\",\n"
      "      \"fields\": [\n"
      "        {\n"
      "          \"name\": \"fld1\",\n"
      "          \"newName\": \"fld1_renamed\",\n"
      "          \"type\": \"Real\",\n"
      "          \"subType\": \"None\",\n"
      "          \"width\": 10,\n"
      "          \"precision\": 3\n"
      "        },\n"
      "        {\n"
      "          \"srcType\": \"String\",\n"
      "          \"type\": \"Integer\"\n"
      "        },\n"
      "        {\n"
      "          \"srcSubType\": \"Boolean\",\n"
      "          \"subType\": \"Boolean\",\n"
      "          \"type\": \"Integer\"\n"
      "        }\n"
      "      ]\n"
      "    }\n"
      "  ]\n"
      "}\n";

  // Load from string
  OGRSchemaOverride schema;
  EXPECT_TRUE(schema.LoadFromJSON(kValidJson));
  EXPECT_TRUE(schema.IsValid());
  EXPECT_EQ(1, schema.GetLayerOverrides().size());

  // Load from file path
  VsiMemTempWrapper json_file("/vsimem/ogr_schema_override_test/schema.json",
                              kValidJson);
  OGRSchemaOverride schema_file;
  EXPECT_TRUE(
      schema_file.LoadFromJSON("/vsimem/ogr_schema_override_test/schema.json"));
  EXPECT_TRUE(schema_file.IsValid());
}

TEST(OgrSchemaOverrideTest, DefaultApply) {
  GDALRegister_MEM();
  auto* driver = GetGDALDriverManager()->GetDriverByName("Memory");
  ASSERT_NE(nullptr, driver);
  std::unique_ptr<GDALDataset> ds(
      driver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  auto* l1 = ds->CreateLayer("l1", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, l1);
  OGRFieldDefn f1("f1", OFTString);
  EXPECT_EQ(OGRERR_NONE, l1->CreateField(&f1));
  OGRFieldDefn f2("f2", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, l1->CreateField(&f2));
  OGRFieldDefn f3("f3", OFTReal);
  EXPECT_EQ(OGRERR_NONE, l1->CreateField(&f3));

  auto* l2 = ds->CreateLayer("l2", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, l2);
  OGRFieldDefn f_other("f_other", OFTString);
  EXPECT_EQ(OGRERR_NONE, l2->CreateField(&f_other));

  // Patch override on l1
  const std::string kPatchJson =
      "{\n"
      "  \"layers\": [\n"
      "    {\n"
      "      \"name\": \"l1\",\n"
      "      \"fields\": [\n"
      "        {\n"
      "          \"name\": \"f1\",\n"
      "          \"newName\": \"f1_renamed\",\n"
      "          \"type\": \"Integer\",\n"
      "          \"width\": 8\n"
      "        },\n"
      "        {\n"
      "          \"srcType\": \"Real\",\n"
      "          \"type\": \"Real\",\n"
      "          \"precision\": 4\n"
      "        }\n"
      "      ]\n"
      "    }\n"
      "  ]\n"
      "}\n";

  OGRSchemaOverride patch_schema;
  ASSERT_TRUE(patch_schema.LoadFromJSON(kPatchJson));
  EXPECT_TRUE(patch_schema.DefaultApply(ds.get(), "DEBUG_KEY"));

  auto* layer1 = ds->GetLayerByName("l1");
  ASSERT_NE(nullptr, layer1);
  auto* defn = layer1->GetLayerDefn();
  EXPECT_EQ(3, defn->GetFieldCount());
  EXPECT_STREQ("f1_renamed", defn->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(OFTInteger, defn->GetFieldDefn(0)->GetType());
  EXPECT_EQ(8, defn->GetFieldDefn(0)->GetWidth());
  EXPECT_EQ(4, defn->GetFieldDefn(2)->GetPrecision());

  // Full override on l1 (removes non-matching fields)
  int removed_count = 0;
  const std::string kFullJson =
      "{\n"
      "  \"layers\": [\n"
      "    {\n"
      "      \"name\": \"l1\",\n"
      "      \"schemaType\": \"Full\",\n"
      "      \"fields\": [\n"
      "        {\n"
      "          \"name\": \"f1_renamed\"\n"
      "        }\n"
      "      ]\n"
      "    }\n"
      "  ]\n"
      "}\n";

  OGRSchemaOverride full_schema;
  ASSERT_TRUE(full_schema.LoadFromJSON(kFullJson));
  EXPECT_TRUE(full_schema.DefaultApply(
      ds.get(), "DEBUG_KEY",
      [&removed_count](OGRLayer*, int) { removed_count++; }));
  EXPECT_EQ(2, removed_count);  // f2 and f3 removed
  EXPECT_EQ(1, defn->GetFieldCount());

  // Wildcard layer name "*"
  const std::string kWildcardJson =
      "{\n"
      "  \"layers\": [\n"
      "    {\n"
      "      \"name\": \"*\",\n"
      "      \"fields\": [\n"
      "        {\n"
      "          \"srcType\": \"String\",\n"
      "          \"width\": 25\n"
      "        }\n"
      "      ]\n"
      "    }\n"
      "  ]\n"
      "}\n";
  OGRSchemaOverride wildcard_schema;
  ASSERT_TRUE(wildcard_schema.LoadFromJSON(kWildcardJson));
  EXPECT_TRUE(wildcard_schema.DefaultApply(ds.get(), "DEBUG_KEY"));

  auto* layer2 = ds->GetLayerByName("l2");
  ASSERT_NE(nullptr, layer2);
  EXPECT_EQ(25, layer2->GetLayerDefn()->GetFieldDefn(0)->GetWidth());

  // Error cases: Layer not found
  WithQuietHandler quiet_handler;
  const std::string kMissingLayerJson =
      "{\"layers\": [{\"name\": \"missing_layer\", \"fields\": [{\"name\": "
      "\"x\", \"type\": \"Real\"}]}]}";
  OGRSchemaOverride missing_layer_schema;
  ASSERT_TRUE(missing_layer_schema.LoadFromJSON(kMissingLayerJson));
  EXPECT_FALSE(missing_layer_schema.DefaultApply(ds.get(), "DEBUG_KEY"));

  // Error cases: Field not found
  const std::string kMissingFieldJson =
      "{\"layers\": [{\"name\": \"l2\", \"fields\": [{\"name\": "
      "\"nonexistent_field\", \"type\": \"Real\"}]}]}";
  OGRSchemaOverride missing_field_schema;
  ASSERT_TRUE(missing_field_schema.LoadFromJSON(kMissingFieldJson));
  EXPECT_FALSE(missing_field_schema.DefaultApply(ds.get(), "DEBUG_KEY"));
}

}  // namespace
}  // namespace autotest2
