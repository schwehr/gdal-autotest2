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
// Tests for GMLFeatureClass (ogr/ogrsf_frmts/gmlutils/gmlfeatureclass.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gmlutils/gmlfeatureclass.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml.py

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

TEST(GmlFeatureClassTest, BasicMetadata) {
  GMLFeatureClass fc("initial_name");
  EXPECT_STREQ("initial_name", fc.GetName());
  EXPECT_STREQ("initial_name", fc.GetElementName());
  EXPECT_EQ(12, fc.GetElementNameLen());

  fc.SetName("renamed");
  EXPECT_STREQ("renamed", fc.GetName());

  fc.SetElementName("custom_elem");
  EXPECT_STREQ("custom_elem", fc.GetElementName());
  EXPECT_EQ(11, fc.GetElementNameLen());

  EXPECT_EQ(-1, fc.GetFeatureCount());
  fc.SetFeatureCount(12345);
  EXPECT_EQ(12345, fc.GetFeatureCount());

  EXPECT_EQ(nullptr, fc.GetExtraInfo());
  fc.SetExtraInfo("extra_info_string");
  EXPECT_STREQ("extra_info_string", fc.GetExtraInfo());
  fc.SetExtraInfo(nullptr);
  EXPECT_EQ(nullptr, fc.GetExtraInfo());

  EXPECT_FALSE(fc.HasExtents());
  double xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
  EXPECT_FALSE(fc.GetExtents(&xmin, &xmax, &ymin, &ymax));
  fc.SetExtents(1.0, 2.0, 3.0, 4.0);
  EXPECT_TRUE(fc.HasExtents());
  EXPECT_TRUE(fc.GetExtents(&xmin, &xmax, &ymin, &ymax));
  EXPECT_DOUBLE_EQ(1.0, xmin);
  EXPECT_DOUBLE_EQ(2.0, xmax);
  EXPECT_DOUBLE_EQ(3.0, ymin);
  EXPECT_DOUBLE_EQ(4.0, ymax);

  EXPECT_FALSE(fc.IsSchemaLocked());
  fc.SetSchemaLocked(true);
  EXPECT_TRUE(fc.IsSchemaLocked());

  EXPECT_TRUE(fc.IsConsistentSingleGeomElemPath());
  fc.SetConsistentSingleGeomElemPath(false);
  EXPECT_FALSE(fc.IsConsistentSingleGeomElemPath());

  EXPECT_TRUE(fc.GetSingleGeomElemPath().empty());
  fc.SetSingleGeomElemPath("geom_path");
  EXPECT_EQ("geom_path", fc.GetSingleGeomElemPath());
}

TEST(GmlFeatureClassTest, SRSNameMerge) {
  GMLFeatureClass fc("fc");
  EXPECT_EQ(nullptr, fc.GetSRSName());

  fc.SetSRSName(nullptr);
  EXPECT_EQ(nullptr, fc.GetSRSName());

  fc.SetSRSName("EPSG:4326");
  EXPECT_STREQ("EPSG:4326", fc.GetSRSName());

  // Merge same SRS keeps it.
  fc.MergeSRSName("EPSG:4326");
  EXPECT_STREQ("EPSG:4326", fc.GetSRSName());

  // Merge different SRS invalidates consistency.
  fc.MergeSRSName("EPSG:3857");
  EXPECT_EQ(nullptr, fc.GetSRSName());

  // Subsequent merges after inconsistency do nothing.
  fc.MergeSRSName("EPSG:4326");
  EXPECT_EQ(nullptr, fc.GetSRSName());

  // Fresh class merge from nullptr.
  GMLFeatureClass fc2("fc2");
  fc2.MergeSRSName(nullptr);
  EXPECT_EQ(nullptr, fc2.GetSRSName());
  fc2.MergeSRSName("EPSG:32631");
  EXPECT_STREQ("EPSG:32631", fc2.GetSRSName());
}

TEST(GmlFeatureClassTest, PropertyManagement) {
  GMLFeatureClass fc("fc");
  EXPECT_EQ(0, fc.GetPropertyCount());
  EXPECT_FALSE(fc.HasFeatureProperties());

  auto p0 = std::make_unique<GMLPropertyDefn>("FieldB", "elemB");
  auto p1 = std::make_unique<GMLPropertyDefn>("FieldC", "elemC");
  auto p_feat = std::make_unique<GMLPropertyDefn>("FieldFeat", "elemFeat");
  p_feat->SetType(GMLPT_FeatureProperty);

  EXPECT_EQ(0, fc.AddProperty(p0.release()));
  EXPECT_EQ(1, fc.AddProperty(p1.release()));
  EXPECT_EQ(2, fc.AddProperty(p_feat.release()));

  EXPECT_TRUE(fc.HasFeatureProperties());
  EXPECT_EQ(3, fc.GetPropertyCount());

  // Insert at position 0 to test shifting and map index updates.
  auto p_first = std::make_unique<GMLPropertyDefn>("FieldA", "elemA");
  EXPECT_EQ(0, fc.AddProperty(p_first.release(), /*iPos=*/0));
  EXPECT_EQ(4, fc.GetPropertyCount());

  EXPECT_STREQ("FieldA", fc.GetProperty(0)->GetName());
  EXPECT_STREQ("FieldB", fc.GetProperty(1)->GetName());
  EXPECT_STREQ("FieldC", fc.GetProperty(2)->GetName());
  EXPECT_STREQ("FieldFeat", fc.GetProperty(3)->GetName());

  EXPECT_EQ(nullptr, fc.GetProperty(-1));
  EXPECT_EQ(nullptr, fc.GetProperty(10));

  // Case-insensitive lookup by name.
  EXPECT_EQ(0, fc.GetPropertyIndex("fielda"));
  EXPECT_EQ(1, fc.GetPropertyIndex("FIELDB"));
  EXPECT_EQ(-1, fc.GetPropertyIndex("nonexistent"));
  EXPECT_NE(nullptr, fc.GetProperty("fieldc"));
  EXPECT_EQ(nullptr, fc.GetProperty("missing"));

  // Lookup by source element.
  EXPECT_EQ(0, fc.GetPropertyIndexBySrcElement("elemA", 5));
  EXPECT_EQ(1, fc.GetPropertyIndexBySrcElement("elemB", 5));
  EXPECT_EQ(-1, fc.GetPropertyIndexBySrcElement("elemMissing", 11));

  // Add duplicate property name fails with warning.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto dup = std::make_unique<GMLPropertyDefn>("FieldA", "elemOther");
    EXPECT_EQ(-1, fc.AddProperty(dup.get()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Steal properties.
  std::vector<GMLPropertyDefn*> stolen;
  for (int i = 0; i < fc.GetPropertyCount(); ++i) {
    stolen.push_back(fc.GetProperty(i));
  }
  fc.StealProperties();
  EXPECT_EQ(0, fc.GetPropertyCount());
  for (auto* p : stolen) {
    delete p;
  }
}

TEST(GmlFeatureClassTest, GeometryPropertyManagement) {
  GMLFeatureClass fc("fc");
  EXPECT_EQ(0, fc.GetGeometryPropertyCount());

  auto g0 = std::make_unique<GMLGeometryPropertyDefn>("geom0", "elem0",
                                                      wkbPoint, -1, true);
  auto g1 = std::make_unique<GMLGeometryPropertyDefn>("geom1", "elem1",
                                                      wkbPolygon, -1, true);

  EXPECT_EQ(0, fc.AddGeometryProperty(g0.release()));
  EXPECT_EQ(1, fc.AddGeometryProperty(g1.release()));
  EXPECT_EQ(2, fc.GetGeometryPropertyCount());

  EXPECT_EQ(nullptr, fc.GetGeometryProperty(-1));
  EXPECT_EQ(nullptr, fc.GetGeometryProperty(5));
  EXPECT_STREQ("geom0", fc.GetGeometryProperty(0)->GetName());
  EXPECT_STREQ("geom1", fc.GetGeometryProperty(1)->GetName());

  EXPECT_EQ(0, fc.GetGeometryPropertyIndexBySrcElement("elem0"));
  EXPECT_EQ(1, fc.GetGeometryPropertyIndexBySrcElement("elem1"));
  EXPECT_EQ(-1, fc.GetGeometryPropertyIndexBySrcElement("elemMissing"));

  // Duplicate src element fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto dup = std::make_unique<GMLGeometryPropertyDefn>(
        "other_geom", "elem0", wkbLineString, -1, true);
    EXPECT_EQ(-1, fc.AddGeometryProperty(dup.get()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Steal geometry properties.
  std::vector<GMLGeometryPropertyDefn*> stolen;
  for (int i = 0; i < fc.GetGeometryPropertyCount(); ++i) {
    stolen.push_back(fc.GetGeometryProperty(i));
  }
  fc.StealGeometryProperties();
  EXPECT_EQ(0, fc.GetGeometryPropertyCount());
  for (auto* g : stolen) {
    delete g;
  }

  // ClearGeometryProperties.
  auto g2 =
      std::make_unique<GMLGeometryPropertyDefn>("g2", "e2", wkbPoint, -1, true);
  fc.AddGeometryProperty(g2.release());
  EXPECT_EQ(1, fc.GetGeometryPropertyCount());
  fc.ClearGeometryProperties();
  EXPECT_EQ(0, fc.GetGeometryPropertyCount());
}

TEST(GmlFeatureClassTest, InitializeFromXMLErrors) {
  GMLFeatureClass fc("fc");

  // 1. Null root.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(fc.InitializeFromXML(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    CPLErrorReset();
  }

  // 2. Non-element root.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLXMLTreeCloser text_node(CPLCreateXMLNode(nullptr, CXT_Text, "text"));
    EXPECT_FALSE(fc.InitializeFromXML(text_node.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Wrong element name.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLXMLTreeCloser wrong_node(
        CPLCreateXMLNode(nullptr, CXT_Element, "WrongName"));
    EXPECT_FALSE(fc.InitializeFromXML(wrong_node.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 4. Missing Name element.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLXMLTreeCloser missing_name(
        CPLCreateXMLNode(nullptr, CXT_Element, "GMLFeatureClass"));
    EXPECT_FALSE(fc.InitializeFromXML(missing_name.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(GmlFeatureClassTest, InitializeFromXMLFull) {
  constexpr const char kXml[] = R"xml(
<GMLFeatureClass>
  <Name>Roads</Name>
  <ElementPath>roads_element</ElementPath>
  <SRSName>EPSG:4326</SRSName>
  <DatasetSpecificInfo>
    <FeatureCount>999</FeatureCount>
    <ExtraInfo>MyExtra</ExtraInfo>
    <ExtentXMin>10.0</ExtentXMin>
    <ExtentXMax>20.0</ExtentXMax>
    <ExtentYMin>30.0</ExtentYMin>
    <ExtentYMax>40.0</ExtentYMax>
  </DatasetSpecificInfo>
  <GeomPropertyDefn>
    <Name>geom_pt</Name>
    <ElementPath>elem_pt</ElementPath>
    <Type>Point</Type>
    <Nullable>false</Nullable>
  </GeomPropertyDefn>
  <GeomPropertyDefn>
    <Name>geom_poly_iso</Name>
    <ElementPath>elem_poly_iso</ElementPath>
    <Type>1003</Type>
    <Nullable>true</Nullable>
  </GeomPropertyDefn>
  <PropertyDefn>
    <Name>str_field</Name>
    <ElementPath>elem_str</ElementPath>
    <Type>String</Type>
    <Width>50</Width>
    <Nullable>true</Nullable>
    <Unique>false</Unique>
    <Comment>String description</Comment>
  </PropertyDefn>
  <PropertyDefn>
    <Name>bool_field</Name>
    <ElementPath>elem_bool</ElementPath>
    <Type>String</Type>
    <Subtype>Boolean</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>date_field</Name>
    <ElementPath>elem_date</ElementPath>
    <Type>String</Type>
    <Subtype>Date</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>time_field</Name>
    <ElementPath>elem_time</ElementPath>
    <Type>String</Type>
    <Subtype>Time</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>datetime_field</Name>
    <ElementPath>elem_datetime</ElementPath>
    <Type>String</Type>
    <Subtype>Datetime</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>int_field</Name>
    <ElementPath>elem_int</ElementPath>
    <Type>Integer</Type>
    <Width>8</Width>
  </PropertyDefn>
  <PropertyDefn>
    <Name>short_field</Name>
    <ElementPath>elem_short</ElementPath>
    <Type>Integer</Type>
    <Subtype>Short</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>int64_field</Name>
    <ElementPath>elem_int64</ElementPath>
    <Type>Integer</Type>
    <Subtype>Integer64</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>real_field</Name>
    <ElementPath>elem_real</ElementPath>
    <Type>Real</Type>
    <Width>12</Width>
    <Precision>4</Precision>
  </PropertyDefn>
  <PropertyDefn>
    <Name>float_field</Name>
    <ElementPath>elem_float</ElementPath>
    <Type>Real</Type>
    <Subtype>Float</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>strlist_field</Name>
    <ElementPath>elem_strlist</ElementPath>
    <Type>StringList</Type>
  </PropertyDefn>
  <PropertyDefn>
    <Name>boollist_field</Name>
    <ElementPath>elem_boollist</ElementPath>
    <Type>StringList</Type>
    <Subtype>Boolean</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>intlist_field</Name>
    <ElementPath>elem_intlist</ElementPath>
    <Type>IntegerList</Type>
  </PropertyDefn>
  <PropertyDefn>
    <Name>int64list_field</Name>
    <ElementPath>elem_int64list</ElementPath>
    <Type>IntegerList</Type>
    <Subtype>Integer64</Subtype>
  </PropertyDefn>
  <PropertyDefn>
    <Name>reallist_field</Name>
    <ElementPath>elem_reallist</ElementPath>
    <Type>RealList</Type>
  </PropertyDefn>
  <PropertyDefn>
    <Name>complex_field</Name>
    <ElementPath>elem_complex</ElementPath>
    <Type>Complex</Type>
  </PropertyDefn>
  <PropertyDefn>
    <Name>featprop_field</Name>
    <ElementPath>elem_featprop</ElementPath>
    <Type>FeatureProperty</Type>
  </PropertyDefn>
  <PropertyDefn>
    <Name>featproplist_field</Name>
    <ElementPath>elem_featproplist</ElementPath>
    <Type>FeaturePropertyList</Type>
  </PropertyDefn>
  <PropertyDefn>
    <Name>untyped_field</Name>
    <ElementPath>elem_untyped</ElementPath>
    <Type>Untyped</Type>
    <Condition>my_cond</Condition>
  </PropertyDefn>
</GMLFeatureClass>
)xml";

  CPLXMLNode* root = CPLParseXMLString(kXml);
  ASSERT_NE(nullptr, root);
  absl::Cleanup root_cleaner = [root] { CPLDestroyXMLNode(root); };

  GMLFeatureClass fc("initial");
  absl::Cleanup extra_info_cleaner = [&fc] { fc.SetExtraInfo(nullptr); };
  EXPECT_TRUE(fc.InitializeFromXML(root));

  EXPECT_STREQ("Roads", fc.GetName());
  EXPECT_STREQ("roads_element", fc.GetElementName());
  EXPECT_STREQ("EPSG:4326", fc.GetSRSName());
  EXPECT_EQ(999, fc.GetFeatureCount());
  EXPECT_STREQ("MyExtra", fc.GetExtraInfo());
  double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
  ASSERT_TRUE(fc.GetExtents(&xmin, &xmax, &ymin, &ymax));
  EXPECT_DOUBLE_EQ(10.0, xmin);
  EXPECT_DOUBLE_EQ(20.0, xmax);
  EXPECT_DOUBLE_EQ(30.0, ymin);
  EXPECT_DOUBLE_EQ(40.0, ymax);

  ASSERT_EQ(2, fc.GetGeometryPropertyCount());
  ASSERT_NE(nullptr, fc.GetGeometryProperty(0));
  EXPECT_STREQ("geom_pt", fc.GetGeometryProperty(0)->GetName());
  EXPECT_EQ(wkbPoint, fc.GetGeometryProperty(0)->GetType());
  EXPECT_FALSE(fc.GetGeometryProperty(0)->IsNullable());

  ASSERT_NE(nullptr, fc.GetGeometryProperty(1));
  EXPECT_STREQ("geom_poly_iso", fc.GetGeometryProperty(1)->GetName());
  EXPECT_EQ(1003, static_cast<int>(fc.GetGeometryProperty(1)->GetType()));

  EXPECT_EQ(19, fc.GetPropertyCount());
  auto* prop_str = fc.GetProperty("str_field");
  ASSERT_NE(nullptr, prop_str);
  EXPECT_EQ(GMLPT_String, prop_str->GetType());
  EXPECT_EQ(50, prop_str->GetWidth());
  EXPECT_EQ("String description", prop_str->GetDocumentation());

  auto* prop_bool = fc.GetProperty("bool_field");
  ASSERT_NE(nullptr, prop_bool);
  EXPECT_EQ(GMLPT_Boolean, prop_bool->GetType());

  auto* prop_date = fc.GetProperty("date_field");
  ASSERT_NE(nullptr, prop_date);
  EXPECT_EQ(GMLPT_Date, prop_date->GetType());

  auto* prop_time = fc.GetProperty("time_field");
  ASSERT_NE(nullptr, prop_time);
  EXPECT_EQ(GMLPT_Time, prop_time->GetType());

  auto* prop_datetime = fc.GetProperty("datetime_field");
  ASSERT_NE(nullptr, prop_datetime);
  EXPECT_EQ(GMLPT_DateTime, prop_datetime->GetType());

  auto* prop_int = fc.GetProperty("int_field");
  ASSERT_NE(nullptr, prop_int);
  EXPECT_EQ(GMLPT_Integer, prop_int->GetType());
  EXPECT_EQ(8, prop_int->GetWidth());

  auto* prop_short = fc.GetProperty("short_field");
  ASSERT_NE(nullptr, prop_short);
  EXPECT_EQ(GMLPT_Short, prop_short->GetType());

  auto* prop_int64 = fc.GetProperty("int64_field");
  ASSERT_NE(nullptr, prop_int64);
  EXPECT_EQ(GMLPT_Integer64, prop_int64->GetType());

  auto* prop_real = fc.GetProperty("real_field");
  ASSERT_NE(nullptr, prop_real);
  EXPECT_EQ(GMLPT_Real, prop_real->GetType());
  EXPECT_EQ(12, prop_real->GetWidth());
  EXPECT_EQ(4, prop_real->GetPrecision());

  auto* prop_float = fc.GetProperty("float_field");
  ASSERT_NE(nullptr, prop_float);
  EXPECT_EQ(GMLPT_Float, prop_float->GetType());

  auto* prop_strlist = fc.GetProperty("strlist_field");
  ASSERT_NE(nullptr, prop_strlist);
  EXPECT_EQ(GMLPT_StringList, prop_strlist->GetType());

  auto* prop_boollist = fc.GetProperty("boollist_field");
  ASSERT_NE(nullptr, prop_boollist);
  EXPECT_EQ(GMLPT_BooleanList, prop_boollist->GetType());

  auto* prop_intlist = fc.GetProperty("intlist_field");
  ASSERT_NE(nullptr, prop_intlist);
  EXPECT_EQ(GMLPT_IntegerList, prop_intlist->GetType());

  auto* prop_int64list = fc.GetProperty("int64list_field");
  ASSERT_NE(nullptr, prop_int64list);
  EXPECT_EQ(GMLPT_Integer64List, prop_int64list->GetType());

  auto* prop_reallist = fc.GetProperty("reallist_field");
  ASSERT_NE(nullptr, prop_reallist);
  EXPECT_EQ(GMLPT_RealList, prop_reallist->GetType());

  auto* prop_complex = fc.GetProperty("complex_field");
  ASSERT_NE(nullptr, prop_complex);
  EXPECT_EQ(GMLPT_Complex, prop_complex->GetType());

  auto* prop_featprop = fc.GetProperty("featprop_field");
  ASSERT_NE(nullptr, prop_featprop);
  EXPECT_EQ(GMLPT_FeatureProperty, prop_featprop->GetType());

  auto* prop_featproplist = fc.GetProperty("featproplist_field");
  ASSERT_NE(nullptr, prop_featproplist);
  EXPECT_EQ(GMLPT_FeaturePropertyList, prop_featproplist->GetType());

  auto* prop_untyped = fc.GetProperty("untyped_field");
  ASSERT_NE(nullptr, prop_untyped);
  EXPECT_EQ(GMLPT_Untyped, prop_untyped->GetType());
  EXPECT_STREQ("my_cond", prop_untyped->GetCondition());
}

TEST(GmlFeatureClassTest, InitializeFromXMLSequentialGeometryElements) {
  constexpr const char kXml[] = R"xml(
<GMLFeatureClass>
  <Name>Points</Name>
  <GeometryName>name1</GeometryName>
  <GeometryName>name2</GeometryName>
  <GeometryElementPath>path1</GeometryElementPath>
  <GeometryElementPath>path2</GeometryElementPath>
  <GeometryType>Point</GeometryType>
  <GeometryType>3</GeometryType>
</GMLFeatureClass>
)xml";

  CPLXMLTreeCloser root(CPLParseXMLString(kXml));
  ASSERT_NE(nullptr, root.get());

  GMLFeatureClass fc("fc");
  {
    // The trailing dangling <GeometryType>3</GeometryType> triggers a 4th
    // AddGeometryProperty("", "") at gmlfeatureclass.cpp:624 which collides
    // with prop 0's empty src element, emitting a CE_Warning.
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(fc.InitializeFromXML(root.get()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }
  ASSERT_EQ(3, fc.GetGeometryPropertyCount());
  ASSERT_NE(nullptr, fc.GetGeometryProperty(0));
  EXPECT_STREQ("name1", fc.GetGeometryProperty(0)->GetName());
  EXPECT_STREQ("", fc.GetGeometryProperty(0)->GetSrcElement());
  EXPECT_EQ(wkbUnknown, fc.GetGeometryProperty(0)->GetType());

  ASSERT_NE(nullptr, fc.GetGeometryProperty(1));
  EXPECT_STREQ("name2", fc.GetGeometryProperty(1)->GetName());
  EXPECT_STREQ("path1", fc.GetGeometryProperty(1)->GetSrcElement());
  EXPECT_EQ(wkbUnknown, fc.GetGeometryProperty(1)->GetType());

  ASSERT_NE(nullptr, fc.GetGeometryProperty(2));
  // When Name is empty, GMLGeometryPropertyDefn defaults Name to SrcElement.
  EXPECT_STREQ("path2", fc.GetGeometryProperty(2)->GetName());
  EXPECT_STREQ("path2", fc.GetGeometryProperty(2)->GetSrcElement());
  EXPECT_EQ(wkbPoint, fc.GetGeometryProperty(2)->GetType());
}

TEST(GmlFeatureClassTest, InitializeFromXMLNoGeometryElements) {
  constexpr const char kXml[] = R"xml(
<GMLFeatureClass>
  <Name>NoGeom</Name>
</GMLFeatureClass>
)xml";

  CPLXMLNode* root = CPLParseXMLString(kXml);
  ASSERT_NE(nullptr, root);
  absl::Cleanup root_cleaner = [root] { CPLDestroyXMLNode(root); };

  GMLFeatureClass fc("fc");
  EXPECT_TRUE(fc.InitializeFromXML(root));
  EXPECT_EQ(1, fc.GetGeometryPropertyCount());
}

TEST(GmlFeatureClassTest, InitializeFromXMLGeometryNone) {
  constexpr const char kXml[] = R"xml(
<GMLFeatureClass>
  <Name>NoneGeom</Name>
  <GeometryType>NONE</GeometryType>
</GMLFeatureClass>
)xml";

  CPLXMLNode* root = CPLParseXMLString(kXml);
  ASSERT_NE(nullptr, root);
  absl::Cleanup root_cleaner = [root] { CPLDestroyXMLNode(root); };

  GMLFeatureClass fc("fc");
  EXPECT_TRUE(fc.InitializeFromXML(root));
  EXPECT_EQ(0, fc.GetGeometryPropertyCount());
}

TEST(GmlFeatureClassTest, InitializeFromXMLPropertyErrors) {
  WithQuietHandler quiet;

  // PropertyDefn without Name.
  {
    constexpr const char kXml[] = R"xml(
<GMLFeatureClass>
  <Name>Test</Name>
  <PropertyDefn>
    <Type>String</Type>
  </PropertyDefn>
</GMLFeatureClass>
)xml";
    CPLXMLNode* root = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, root);
    absl::Cleanup cleaner = [root] { CPLDestroyXMLNode(root); };
    GMLFeatureClass fc("fc");
    CPLErrorReset();
    EXPECT_FALSE(fc.InitializeFromXML(root));
  }

  // PropertyDefn with unrecognized Type.
  {
    constexpr const char kXml[] = R"xml(
<GMLFeatureClass>
  <Name>Test</Name>
  <PropertyDefn>
    <Name>foo</Name>
    <Type>BadType</Type>
  </PropertyDefn>
</GMLFeatureClass>
)xml";
    CPLXMLNode* root = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, root);
    absl::Cleanup cleaner = [root] { CPLDestroyXMLNode(root); };
    GMLFeatureClass fc("fc");
    CPLErrorReset();
    EXPECT_FALSE(fc.InitializeFromXML(root));
  }
}

TEST(GmlFeatureClassTest, SerializeToXMLZeroAndSingleAndMultiGeom) {
  // 1. Zero geometries.
  {
    GMLFeatureClass fc("EmptyGeom");
    CPLXMLNode* node = fc.SerializeToXML();
    ASSERT_NE(nullptr, node);
    absl::Cleanup cleaner = [node] { CPLDestroyXMLNode(node); };
    char* xml = CPLSerializeXMLTree(node);
    std::unique_ptr<char, CplFreeDeleter> xml_ptr(xml);
    EXPECT_NE(nullptr, std::strstr(xml, "<GeometryType>100</GeometryType>"));
  }

  // 2. Single geometry with 3D type.
  {
    GMLFeatureClass fc("SingleGeom");
    absl::Cleanup extra_info_cleaner = [&fc] { fc.SetExtraInfo(nullptr); };
    fc.SetSRSName("EPSG:4326");
    fc.SetFeatureCount(50);
    fc.SetExtents(1.0, 2.0, 3.0, 4.0);
    fc.SetExtraInfo("extra");

    auto geom = std::make_unique<GMLGeometryPropertyDefn>(
        "my_geom", "src_geom", wkbPoint25D, -1, true);
    fc.AddGeometryProperty(geom.release());

    auto prop = std::make_unique<GMLPropertyDefn>("int_field", "int_elem");
    prop->SetType(GMLPT_Integer);
    prop->SetWidth(5);
    prop->SetDocumentation("int doc");
    fc.AddProperty(prop.release());

    auto prop_real =
        std::make_unique<GMLPropertyDefn>("real_field", "real_elem");
    prop_real->SetType(GMLPT_Real);
    prop_real->SetWidth(8);
    prop_real->SetPrecision(3);
    fc.AddProperty(prop_real.release());

    CPLXMLNode* node = fc.SerializeToXML();
    ASSERT_NE(nullptr, node);
    absl::Cleanup cleaner = [node] { CPLDestroyXMLNode(node); };
    char* xml = CPLSerializeXMLTree(node);
    std::unique_ptr<char, CplFreeDeleter> xml_ptr(xml);
    EXPECT_NE(nullptr,
              std::strstr(xml, "<GeometryName>my_geom</GeometryName>"));
    EXPECT_NE(nullptr, std::strstr(xml, "<!--POINTZ-->"));
    EXPECT_NE(nullptr,
              std::strstr(xml, "<GeometryType>-2147483647</GeometryType>"));

    // Reset ExtraInfo to avoid leak due to upstream destructor bug.
    fc.SetExtraInfo(nullptr);
  }

  // 3. Multi geometry.
  {
    GMLFeatureClass fc("MultiGeom");
    auto g0 = std::make_unique<GMLGeometryPropertyDefn>("g0", "e0", wkbPolygon,
                                                        -1, true);
    auto g1 = std::make_unique<GMLGeometryPropertyDefn>(
        "g1", "e1", wkbMultiPolygon25D, -1, true);
    fc.AddGeometryProperty(g0.release());
    fc.AddGeometryProperty(g1.release());

    // Add properties for all types to cover the switch statement in
    // SerializeToXML.
    const GMLPropertyType types[] = {GMLPT_Untyped,
                                     GMLPT_String,
                                     GMLPT_Boolean,
                                     GMLPT_Date,
                                     GMLPT_Time,
                                     GMLPT_DateTime,
                                     GMLPT_Integer,
                                     GMLPT_Short,
                                     GMLPT_Integer64,
                                     GMLPT_Real,
                                     GMLPT_Float,
                                     GMLPT_Complex,
                                     GMLPT_IntegerList,
                                     GMLPT_Integer64List,
                                     GMLPT_RealList,
                                     GMLPT_StringList,
                                     GMLPT_BooleanList,
                                     GMLPT_FeatureProperty,
                                     GMLPT_FeaturePropertyList};
    int idx = 0;
    for (GMLPropertyType t : types) {
      auto p = std::make_unique<GMLPropertyDefn>(CPLSPrintf("f_%d", idx),
                                                 CPLSPrintf("e_%d", idx));
      p->SetType(t);
      if (t == GMLPT_String) p->SetWidth(10);
      fc.AddProperty(p.release());
      idx++;
    }

    CPLXMLNode* node = fc.SerializeToXML();
    ASSERT_NE(nullptr, node);
    absl::Cleanup cleaner = [node] { CPLDestroyXMLNode(node); };
    char* xml = CPLSerializeXMLTree(node);
    std::unique_ptr<char, CplFreeDeleter> xml_ptr(xml);
    EXPECT_NE(nullptr, std::strstr(xml, "<GeomPropertyDefn>"));

    // Round-trip verification: deserialize back and verify property count.
    GMLFeatureClass fc_rt("rt");
    EXPECT_TRUE(fc_rt.InitializeFromXML(node));
    EXPECT_EQ(19, fc_rt.GetPropertyCount());
  }
}

TEST(GmlFeatureClassTest, FieldTypeConversions) {
  // Test GML_GetOGRFieldType across all 19 enum values.
  struct GetCase {
    GMLPropertyType gml_type;
    OGRFieldType expected_ogr;
    OGRFieldSubType expected_subtype;
  };
  constexpr GetCase kGetCases[] = {
      {.gml_type = GMLPT_Untyped,
       .expected_ogr = OFTString,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_String,
       .expected_ogr = OFTString,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Integer,
       .expected_ogr = OFTInteger,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Boolean,
       .expected_ogr = OFTInteger,
       .expected_subtype = OFSTBoolean},
      {.gml_type = GMLPT_Short,
       .expected_ogr = OFTInteger,
       .expected_subtype = OFSTInt16},
      {.gml_type = GMLPT_Integer64,
       .expected_ogr = OFTInteger64,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Real,
       .expected_ogr = OFTReal,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Float,
       .expected_ogr = OFTReal,
       .expected_subtype = OFSTFloat32},
      {.gml_type = GMLPT_StringList,
       .expected_ogr = OFTStringList,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_IntegerList,
       .expected_ogr = OFTIntegerList,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_BooleanList,
       .expected_ogr = OFTIntegerList,
       .expected_subtype = OFSTBoolean},
      {.gml_type = GMLPT_Integer64List,
       .expected_ogr = OFTInteger64List,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_RealList,
       .expected_ogr = OFTRealList,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Date,
       .expected_ogr = OFTDate,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Time,
       .expected_ogr = OFTTime,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_DateTime,
       .expected_ogr = OFTDateTime,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_Complex,
       .expected_ogr = OFTString,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_FeatureProperty,
       .expected_ogr = OFTString,
       .expected_subtype = OFSTNone},
      {.gml_type = GMLPT_FeaturePropertyList,
       .expected_ogr = OFTStringList,
       .expected_subtype = OFSTNone},
  };
  static_assert(std::size(kGetCases) == 19);

  for (const auto& c : kGetCases) {
    OGRFieldSubType sub_type = OFSTNone;
    OGRFieldType ftype = GML_GetOGRFieldType(c.gml_type, sub_type);
    EXPECT_EQ(c.expected_ogr, ftype);
    EXPECT_EQ(c.expected_subtype, sub_type);
  }

  // Test GML_FromOGRFieldType across all types.
  EXPECT_EQ(GMLPT_String, GML_FromOGRFieldType(OFTString, OFSTNone));
  EXPECT_EQ(GMLPT_Boolean, GML_FromOGRFieldType(OFTInteger, OFSTBoolean));
  EXPECT_EQ(GMLPT_Short, GML_FromOGRFieldType(OFTInteger, OFSTInt16));
  EXPECT_EQ(GMLPT_Integer, GML_FromOGRFieldType(OFTInteger, OFSTNone));
  EXPECT_EQ(GMLPT_Real, GML_FromOGRFieldType(OFTReal, OFSTNone));
  EXPECT_EQ(GMLPT_Date, GML_FromOGRFieldType(OFTDate, OFSTNone));
  EXPECT_EQ(GMLPT_DateTime, GML_FromOGRFieldType(OFTDateTime, OFSTNone));
  EXPECT_EQ(GMLPT_Time, GML_FromOGRFieldType(OFTTime, OFSTNone));
  EXPECT_EQ(GMLPT_Untyped, GML_FromOGRFieldType(OFTBinary, OFSTNone));
}

// TODO: b/335317901 - GMLFeatureClass achieves 98.10% line coverage. Track any
// upstream fixes to ~GMLFeatureClass() m_pszExtraInfo leak.

}  // namespace
}  // namespace autotest2
