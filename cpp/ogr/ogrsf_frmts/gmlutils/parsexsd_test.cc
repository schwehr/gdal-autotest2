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
// Tests for GML schema parser (ogr/ogrsf_frmts/gmlutils/parsexsd.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gmlutils/parsexsd.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml.py
//
// Build caveat: libcurl is disabled in google3 (HAVE_CURL not defined), so
// CPLHTTPFetch falls back to the stub returning nullptr with CPLE_NotSupported.

#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/parsexsd.h"

#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_http.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

void CleanClasses(std::vector<GMLFeatureClass*>& classes) {
  for (auto* poClass : classes) {
    delete poClass;
  }
  classes.clear();
}

GMLGeometryPropertyDefn* GetGeomProp(GMLFeatureClass* fc, const char* name) {
  for (int i = 0; i < fc->GetGeometryPropertyCount(); ++i) {
    auto* prop = fc->GetGeometryProperty(i);
    if (prop && std::strcmp(prop->GetName(), name) == 0) {
      return prop;
    }
  }
  return nullptr;
}

GMLFeatureClass* FindClass(const std::vector<GMLFeatureClass*>& classes,
                           const char* name) {
  for (auto* fc : classes) {
    if (fc && fc->GetName() && std::strcmp(fc->GetName(), name) == 0) {
      return fc;
    }
  }
  return nullptr;
}

TEST(ParseXsdTest, NullAndMissingFiles) {
  std::vector<GMLFeatureClass*> classes;
  bool fully_understood = false;

  // 1. Null file path.
  EXPECT_FALSE(GMLParseXSD(nullptr, false, classes, fully_understood));
  EXPECT_FALSE(fully_understood);

  // 2. Non-existent file.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/nonexistent.xsd", false,
                             classes, fully_understood));
    EXPECT_FALSE(fully_understood);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. XML file without schema root.
  {
    constexpr const char kInvalidXml[] = "<not_a_schema/>";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/invalid.xsd", kInvalidXml);
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/invalid.xsd", false,
                             classes, fully_understood));
    EXPECT_FALSE(fully_understood);
  }
}

TEST(ParseXsdTest, HttpFetchCallback) {
  constexpr const char kHttpXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="HttpFeature" type="HttpFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="HttpFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="prop" type="xs:string"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";

  auto callback = [](const char* pszURL, CSLConstList, GDALProgressFunc, void*,
                     CPLHTTPFetchWriteFunc, void*,
                     void* pUserData) -> CPLHTTPResult* {
    if (std::strstr(pszURL, "null_result") != nullptr) {
      return nullptr;
    }
    auto* xml = static_cast<const char*>(pUserData);
    auto* res =
        static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
    res->pabyData = reinterpret_cast<GByte*>(CPLStrdup(xml));
    res->nDataLen = static_cast<int>(std::strlen(xml));
    return res;
  };

  CPLHTTPPushFetchCallback(callback, const_cast<char*>(kHttpXml));
  absl::Cleanup pop_cb = [] { CPLHTTPPopFetchCallback(); };

  // 1. Success HTTP fetch.
  {
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_TRUE(GMLParseXSD("http://example.com/schema.xsd", false, classes,
                            fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_TRUE(fully_understood);
    ASSERT_EQ(1, classes.size());
    EXPECT_STREQ("HttpFeature", classes[0]->GetName());
    EXPECT_EQ(1, classes[0]->GetPropertyCount());
  }

  // 2. HTTPS fetch returning null result.
  // In google3, libcurl is disabled (HAVE_CURL not defined), so CPLHTTPFetch
  // falls back to the stub returning nullptr with CPLE_NotSupported.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("https://example.com/null_result.xsd", false,
                             classes, fully_understood));
    EXPECT_FALSE(fully_understood);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    CPLErrorReset();
  }
}

TEST(ParseXsdTest, IncludesImportsAndSchemaOutput) {
  // Push a callback to ensure no network calls occur during the test.
  CPLHTTPPushFetchCallback(
      [](const char* pszURL, CSLConstList, GDALProgressFunc, void*,
         CPLHTTPFetchWriteFunc, void*, void*) -> CPLHTTPResult* {
        ADD_FAILURE() << "Unexpected HTTP fetch in hermetic test: " << pszURL;
        return nullptr;
      },
      nullptr);
  absl::Cleanup callback_cleanup = [] { CPLHTTPPopFetchCallback(); };

  constexpr const char kMainXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:include schemaLocation="sub1.xsd"/>
  <xs:import schemaLocation="sub2.xsd"/>
  <xs:import schemaLocation="http://schemas.opengis.net/gml/3.2.1/gml.xsd"/>
</xs:schema>
)xml";

  constexpr const char kSub1Xml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:include schemaLocation="sub_nested.xsd"/>
</xs:schema>
)xml";

  constexpr const char kSubNestedXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="NestedFeature" type="NestedFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="NestedFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="str_prop" type="xs:string"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";

  constexpr const char kSub2Xml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="ImportFeature" type="ImportFeatureType" substitutionGroup="gml:AbstractFeature"/>
  <xs:complexType name="ImportFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="int_prop" type="xs:int"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";

  VsiMemTempWrapper f_main("/vsimem/parsexsd_test/main.xsd", kMainXml);
  VsiMemTempWrapper f_sub1("/vsimem/parsexsd_test/sub1.xsd", kSub1Xml);
  VsiMemTempWrapper f_nested("/vsimem/parsexsd_test/sub_nested.xsd",
                             kSubNestedXml);
  VsiMemTempWrapper f_sub2("/vsimem/parsexsd_test/sub2.xsd", kSub2Xml);

  constexpr const char kOutSchemaFile[] =
      "/vsimem/parsexsd_test/out_schema.xsd";
  CPLSetConfigOption("GML_SCHEMA_OUTPUT_NAME", kOutSchemaFile);
  absl::Cleanup opt_cleanup = [kOutSchemaFile] {
    CPLSetConfigOption("GML_SCHEMA_OUTPUT_NAME", nullptr);
    VSIUnlink(kOutSchemaFile);
  };

  std::vector<GMLFeatureClass*> classes;
  bool fully_understood = false;
  EXPECT_TRUE(GMLParseXSD("/vsimem/parsexsd_test/main.xsd",
                          /*bUseSchemaImports=*/true, classes,
                          fully_understood));
  absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
  EXPECT_TRUE(fully_understood);
  ASSERT_EQ(2, classes.size());
  EXPECT_STREQ("NestedFeature", classes[0]->GetName());
  EXPECT_STREQ("ImportFeature", classes[1]->GetName());

  // Verify that GML_SCHEMA_OUTPUT_NAME wrote the serialized schema.
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(kOutSchemaFile, &sStat));
  EXPECT_GT(sStat.st_size, 0);
}

TEST(ParseXsdTest, ComprehensiveAttributeAndSimpleTypes) {
  constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">

  <!-- Top-level simple types covering all branches in GetSimpleTypeProperties -->
  <xs:simpleType name="dec_type">
    <xs:restriction base="xs:decimal">
      <xs:totalDigits value="10"/>
      <xs:fractionDigits value="3"/>
    </xs:restriction>
  </xs:simpleType>

  <xs:simpleType name="float_type">
    <xs:restriction base="xs:float"/>
  </xs:simpleType>

  <xs:simpleType name="double_type">
    <xs:restriction base="xs:double"/>
  </xs:simpleType>

  <xs:simpleType name="int_type">
    <xs:restriction base="xs:integer">
      <xs:totalDigits value="7"/>
    </xs:restriction>
  </xs:simpleType>

  <xs:simpleType name="long_type">
    <xs:restriction base="xs:long">
      <xs:totalDigits value="15"/>
    </xs:restriction>
  </xs:simpleType>

  <xs:simpleType name="ulong_type">
    <xs:restriction base="xs:unsignedLong">
      <xs:totalDigits value="16"/>
    </xs:restriction>
  </xs:simpleType>

  <xs:simpleType name="str_type">
    <xs:restriction base="xs:string">
      <xs:maxLength value="30"/>
    </xs:restriction>
  </xs:simpleType>

  <xs:simpleType name="date_type">
    <xs:restriction base="xs:date"/>
  </xs:simpleType>

  <xs:simpleType name="time_type">
    <xs:restriction base="xs:time"/>
  </xs:simpleType>

  <xs:simpleType name="datetime_type">
    <xs:restriction base="xs:dateTime"/>
  </xs:simpleType>

  <xs:simpleType name="instant_type">
    <xs:restriction base="gml:TimeInstantType"/>
  </xs:simpleType>

  <xs:simpleType name="bool_type">
    <xs:restriction base="xs:boolean"/>
  </xs:simpleType>

  <xs:simpleType name="short_type">
    <xs:restriction base="xs:short"/>
  </xs:simpleType>

  <xs:element name="AllTypesFeature" type="AllTypesFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="AllTypesFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <!-- Using top-level simple types -->
          <xs:element name="p_dec" type="dec_type"/>
          <xs:element name="p_float" type="float_type"/>
          <xs:element name="p_double" type="double_type"/>
          <xs:element name="p_int" type="int_type"/>
          <xs:element name="p_long" type="long_type"/>
          <xs:element name="p_ulong" type="ulong_type"/>
          <xs:element name="p_str" type="str_type"/>
          <xs:element name="p_date" type="date_type"/>
          <xs:element name="p_time" type="time_type"/>
          <xs:element name="p_datetime" type="datetime_type"/>
          <xs:element name="p_instant" type="instant_type"/>
          <xs:element name="p_bool" type="bool_type"/>
          <xs:element name="p_short" type="short_type"/>

          <!-- Direct element types -->
          <xs:element name="d_char" type="xs:Character"/>
          <xs:element name="d_real" type="xs:real"/>
          <xs:element name="d_hex" type="xs:hexBinary"/>
          <xs:element name="d_featprop" type="gml:FeaturePropertyType"/>

          <!-- List types via maxOccurs="unbounded" -->
          <xs:element name="l_str" type="xs:string" maxOccurs="unbounded"/>
          <xs:element name="l_int" type="xs:int" maxOccurs="unbounded"/>
          <xs:element name="l_long" type="xs:long" maxOccurs="unbounded"/>
          <xs:element name="l_real" type="xs:decimal" maxOccurs="unbounded"/>
          <xs:element name="l_bool" type="xs:boolean" maxOccurs="unbounded"/>
          <xs:element name="l_featprop" type="gml:FeaturePropertyType" maxOccurs="unbounded"/>

          <!-- Inline simpleType with annotation documentation -->
          <xs:element name="inline_str" minOccurs="0" maxOccurs="1">
            <xs:annotation>
              <xs:documentation>Inline documentation test</xs:documentation>
            </xs:annotation>
            <xs:simpleType>
              <xs:restriction base="xs:string">
                <xs:maxLength value="45"/>
              </xs:restriction>
            </xs:simpleType>
          </xs:element>

          <!-- Inline simpleType list -->
          <xs:element name="inline_list" minOccurs="0" maxOccurs="2">
            <xs:simpleType>
              <xs:restriction base="xs:integer">
                <xs:totalDigits value="5"/>
              </xs:restriction>
            </xs:simpleType>
          </xs:element>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";

  VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/all_types.xsd", kXml);
  std::vector<GMLFeatureClass*> classes;
  bool fully_understood = false;
  EXPECT_TRUE(GMLParseXSD("/vsimem/parsexsd_test/all_types.xsd", false, classes,
                          fully_understood));
  absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
  EXPECT_TRUE(fully_understood);
  ASSERT_EQ(1, classes.size());

  GMLFeatureClass* fc = classes[0];
  EXPECT_STREQ("AllTypesFeature", fc->GetName());

  auto check_prop = [fc](const char* name, GMLPropertyType expected_type,
                         int expected_width = 0, int expected_prec = 0) {
    auto* prop = fc->GetProperty(name);
    ASSERT_NE(nullptr, prop) << "Missing property: " << name;
    EXPECT_EQ(expected_type, prop->GetType()) << "Property: " << name;
    if (expected_width > 0) {
      EXPECT_EQ(expected_width, prop->GetWidth()) << "Property: " << name;
    }
    if (expected_prec > 0) {
      EXPECT_EQ(expected_prec, prop->GetPrecision()) << "Property: " << name;
    }
  };

  check_prop("p_dec", GMLPT_Real, 10, 3);
  check_prop("p_float", GMLPT_Float);
  check_prop("p_double", GMLPT_Real);
  check_prop("p_int", GMLPT_Integer, 7);
  check_prop("p_long", GMLPT_Integer64, 15);
  check_prop("p_ulong", GMLPT_Integer64, 16);
  check_prop("p_str", GMLPT_String, 30);
  check_prop("p_date", GMLPT_Date);
  check_prop("p_time", GMLPT_Time);
  check_prop("p_datetime", GMLPT_DateTime);
  check_prop("p_instant", GMLPT_DateTime);
  check_prop("p_bool", GMLPT_Boolean);
  check_prop("p_short", GMLPT_Short);
  check_prop("d_char", GMLPT_String);
  check_prop("d_real", GMLPT_Real);
  check_prop("d_hex", GMLPT_String);
  check_prop("d_featprop_href", GMLPT_FeatureProperty);
  check_prop("l_str", GMLPT_StringList);
  check_prop("l_int", GMLPT_IntegerList);
  check_prop("l_long", GMLPT_Integer64List);
  check_prop("l_real", GMLPT_RealList);
  check_prop("l_bool", GMLPT_BooleanList);
  check_prop("l_featprop_href", GMLPT_FeaturePropertyList);

  auto* inline_str = fc->GetProperty("inline_str");
  ASSERT_NE(nullptr, inline_str);
  EXPECT_EQ(GMLPT_String, inline_str->GetType());
  EXPECT_EQ(45, inline_str->GetWidth());
  EXPECT_EQ("Inline documentation test", inline_str->GetDocumentation());

  auto* inline_list = fc->GetProperty("inline_list");
  ASSERT_NE(nullptr, inline_list);
  EXPECT_EQ(GMLPT_IntegerList, inline_list->GetType());
  EXPECT_EQ(5, inline_list->GetWidth());
}

TEST(ParseXsdTest, GeometryTypesAndCoordinatePrecision) {
  constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml"
           xmlns:G="http://intergraph.com"
           xmlns:gmgml="http://intergraph.com/gmgml"
           xmlns:wfs="http://www.opengis.net/wfs"
           xmlns:ogr="http://ogr.maptools.org/">

  <xs:element name="GeomFeature" type="GeomFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="GeomFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <!-- All gml:*PropertyType -->
          <xs:element name="g_any" type="gml:GeometryPropertyType"/>
          <xs:element name="g_pt" type="gml:PointPropertyType"/>
          <xs:element name="g_line" type="gml:LineStringPropertyType"/>
          <xs:element name="g_curve" type="gml:CurvePropertyType"/>
          <xs:element name="g_poly" type="gml:PolygonPropertyType"/>
          <xs:element name="g_surface" type="gml:SurfacePropertyType">
            <xs:annotation>
              <xs:appinfo source="http://ogr.maptools.org/">
                <ogr:xy_coordinate_resolution>8.9e-9</ogr:xy_coordinate_resolution>
                <ogr:z_coordinate_resolution>1e-3</ogr:z_coordinate_resolution>
              </xs:appinfo>
            </xs:annotation>
          </xs:element>
          <xs:element name="g_mpt" type="gml:MultiPointPropertyType"/>
          <xs:element name="g_mline" type="gml:MultiLineStringPropertyType"/>
          <xs:element name="g_mcurve" type="gml:MultiCurvePropertyType"/>
          <xs:element name="g_mpoly" type="gml:MultiPolygonPropertyType"/>
          <xs:element name="g_msurface" type="gml:MultiSurfacePropertyType"/>
          <xs:element name="g_mgeom" type="gml:MultiGeometryPropertyType"/>
          <xs:element name="g_assoc" type="gml:GeometryAssociationType"/>

          <!-- Intergraph & ERDAS types -->
          <xs:element name="g_ig_pt" type="G:Point_MultiPointPropertyType"/>
          <xs:element name="g_ig_line" type="gmgml:LineString_MultiLineStringPropertyType"/>
          <xs:element name="g_ig_poly" type="G:Polygon_MultiPolygonPropertyType"/>
          <xs:element name="g_erdas_poly" type="wfs:MixedPolygonPropertyType"/>

          <!-- ComplexType sequence wrapping gml:_Geometry -->
          <xs:element name="wrapped_geom" minOccurs="0" maxOccurs="1">
            <xs:complexType>
              <xs:sequence>
                <xs:element ref="gml:_Geometry"/>
              </xs:sequence>
            </xs:complexType>
          </xs:element>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <!-- Separate feature types for restricted comment handling to avoid
       comment scanning across subsequent sequence elements in parsexsd.cpp -->
  <xs:element name="RestrictPoly" type="RestrictPolyType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="RestrictPolyType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="g_restrict_poly" type="gml:GeometryPropertyType"/>
          <!-- restricted to Polygon -->
          <!-- srsName="EPSG:4326" -->
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <xs:element name="RestrictLine" type="RestrictLineType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="RestrictLineType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="g_restrict_line" type="gml:GeometryPropertyType"/>
          <!-- restricted to LineString -->
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <xs:element name="RestrictMPoly" type="RestrictMPolyType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="RestrictMPolyType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="g_restrict_mpoly" type="gml:GeometryPropertyType"/>
          <!-- restricted to MultiPolygon -->
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <xs:element name="RestrictMLine" type="RestrictMLineType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="RestrictMLineType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="g_restrict_mline" type="gml:GeometryPropertyType"/>
          <!-- restricted to MultiLineString -->
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <!-- Choice between polygon and multipolygon -->
  <xs:element name="ChoiceFeature" type="ChoiceFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="ChoiceFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:choice>
            <xs:element ref="gml:polygonProperty"/>
            <xs:element ref="gml:multiPolygonProperty"/>
          </xs:choice>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";

  VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/geom_types.xsd", kXml);
  std::vector<GMLFeatureClass*> classes;
  bool fully_understood = false;
  EXPECT_TRUE(GMLParseXSD("/vsimem/parsexsd_test/geom_types.xsd", false,
                          classes, fully_understood));
  absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
  EXPECT_TRUE(fully_understood);
  ASSERT_EQ(6, classes.size());

  GMLFeatureClass* fc = FindClass(classes, "GeomFeature");
  ASSERT_NE(nullptr, fc);

  EXPECT_EQ(wkbUnknown, GetGeomProp(fc, "g_any")->GetType());
  EXPECT_EQ(wkbPoint, GetGeomProp(fc, "g_pt")->GetType());
  EXPECT_EQ(wkbLineString, GetGeomProp(fc, "g_line")->GetType());
  EXPECT_EQ(wkbCompoundCurve, GetGeomProp(fc, "g_curve")->GetType());
  EXPECT_EQ(wkbPolygon, GetGeomProp(fc, "g_poly")->GetType());
  EXPECT_EQ(wkbCurvePolygon, GetGeomProp(fc, "g_surface")->GetType());

  // Coordinate precision on g_surface.
  EXPECT_DOUBLE_EQ(
      8.9e-9,
      GetGeomProp(fc, "g_surface")->GetCoordinatePrecision().dfXYResolution);
  EXPECT_DOUBLE_EQ(
      1e-3,
      GetGeomProp(fc, "g_surface")->GetCoordinatePrecision().dfZResolution);

  EXPECT_EQ(wkbMultiPoint, GetGeomProp(fc, "g_mpt")->GetType());
  EXPECT_EQ(wkbMultiLineString, GetGeomProp(fc, "g_mline")->GetType());
  EXPECT_EQ(wkbMultiCurve, GetGeomProp(fc, "g_mcurve")->GetType());
  EXPECT_EQ(wkbMultiPolygon, GetGeomProp(fc, "g_mpoly")->GetType());
  EXPECT_EQ(wkbMultiSurface, GetGeomProp(fc, "g_msurface")->GetType());
  EXPECT_EQ(wkbGeometryCollection, GetGeomProp(fc, "g_mgeom")->GetType());
  EXPECT_EQ(wkbUnknown, GetGeomProp(fc, "g_assoc")->GetType());

  EXPECT_EQ(wkbMultiPoint, GetGeomProp(fc, "g_ig_pt")->GetType());
  EXPECT_EQ(wkbMultiLineString, GetGeomProp(fc, "g_ig_line")->GetType());
  EXPECT_EQ(wkbMultiPolygon, GetGeomProp(fc, "g_ig_poly")->GetType());
  EXPECT_EQ(wkbMultiPolygon, GetGeomProp(fc, "g_erdas_poly")->GetType());

  EXPECT_EQ(wkbUnknown, GetGeomProp(fc, "wrapped_geom")->GetType());

  // Restricted comment classes.
  GMLFeatureClass* fc_poly = FindClass(classes, "RestrictPoly");
  ASSERT_NE(nullptr, fc_poly);
  ASSERT_NE(nullptr, GetGeomProp(fc_poly, "g_restrict_poly"));
  EXPECT_EQ(wkbPolygon, GetGeomProp(fc_poly, "g_restrict_poly")->GetType());
  EXPECT_EQ("EPSG:4326", GetGeomProp(fc_poly, "g_restrict_poly")->GetSRSName());

  GMLFeatureClass* fc_line = FindClass(classes, "RestrictLine");
  ASSERT_NE(nullptr, fc_line);
  ASSERT_NE(nullptr, GetGeomProp(fc_line, "g_restrict_line"));
  EXPECT_EQ(wkbLineString, GetGeomProp(fc_line, "g_restrict_line")->GetType());

  GMLFeatureClass* fc_mpoly = FindClass(classes, "RestrictMPoly");
  ASSERT_NE(nullptr, fc_mpoly);
  ASSERT_NE(nullptr, GetGeomProp(fc_mpoly, "g_restrict_mpoly"));
  EXPECT_EQ(wkbMultiPolygon,
            GetGeomProp(fc_mpoly, "g_restrict_mpoly")->GetType());

  GMLFeatureClass* fc_mline = FindClass(classes, "RestrictMLine");
  ASSERT_NE(nullptr, fc_mline);
  ASSERT_NE(nullptr, GetGeomProp(fc_mline, "g_restrict_mline"));
  EXPECT_EQ(wkbMultiLineString,
            GetGeomProp(fc_mline, "g_restrict_mline")->GetType());

  // Choice class.
  GMLFeatureClass* fc_choice = FindClass(classes, "ChoiceFeature");
  ASSERT_NE(nullptr, fc_choice);
  EXPECT_EQ(1, fc_choice->GetGeometryPropertyCount());
  EXPECT_EQ(wkbMultiPolygon, fc_choice->GetGeometryProperty(0)->GetType());
}

TEST(ParseXsdTest, FmeRefGeometryTypesAndUpgrade) {
  constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">

  <xs:element name="FmeFeature" type="FmeFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="FmeFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <!-- Initially point, then upgraded to multiPoint -->
          <xs:element ref="gml:pointProperty"/>
          <xs:element ref="gml:multiPointProperty"/>
          <!-- Duplicate point ignored with warning -->
          <xs:element ref="gml:pointProperty"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <xs:element name="FmeLineFeature" type="FmeLineFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="FmeLineFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element ref="gml:curveProperty"/>
          <xs:element ref="gml:multiCurveProperty"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <xs:element name="FmePolyFeature" type="FmePolyFeatureType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="FmePolyFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element ref="gml:surfaceProperty"/>
          <xs:element ref="gml:multiSurfaceProperty"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";

  VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/fme_ref.xsd", kXml);
  std::vector<GMLFeatureClass*> classes;
  bool fully_understood = false;
  EXPECT_TRUE(GMLParseXSD("/vsimem/parsexsd_test/fme_ref.xsd", false, classes,
                          fully_understood));
  absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
  EXPECT_TRUE(fully_understood);
  ASSERT_EQ(3, classes.size());

  EXPECT_EQ(wkbMultiPoint, classes[0]->GetGeometryProperty(0)->GetType());
  EXPECT_EQ(wkbMultiLineString, classes[1]->GetGeometryProperty(0)->GetType());
  EXPECT_EQ(wkbMultiPolygon, classes[2]->GetGeometryProperty(0)->GetType());
}

TEST(ParseXsdTest, NamingRulesAndUniqueConstraints) {
  constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml"
           xmlns:ogr="http://ogr.maptools.org/"
           xmlns:iwfs="http://example.com/iwfs"
           xmlns:ktjkiiwfs="http://example.com/ktjkiiwfs">

  <!-- FeatureCollection with unique constraints -->
  <xs:element name="FeatureCollection" substitutionGroup="gml:_FeatureCollection">
    <xs:unique name="u_poly">
      <xs:selector xpath="ogr:featureMember/ogr:TargetFeature"/>
      <xs:field xpath="ogr:unique_id"/>
    </xs:unique>
  </xs:element>

  <!-- pszType == pszName -->
  <xs:element name="Air" type="iwfs:Air" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="Air">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="altitude" type="xs:int"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <!-- Prefix matching suffix Type -->
  <xs:element name="RekisteriyksikonPalstanTietoja" type="ktjkiiwfs:PalstanTietojaType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="PalstanTietojaType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="code" type="xs:string"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <!-- Name ending with FeatureType -->
  <xs:element name="TargetFeature" type="TargetFeatureType" substitutionGroup="gml:AbstractFeature"/>
  <xs:complexType name="TargetFeatureType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="unique_id" type="xs:int"/>
          <xs:element name="normal_field" type="xs:string"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <!-- Name ending with _Type -->
  <xs:element name="UnderscoreFeat" type="UnderscoreFeat_Type" substitutionGroup="gml:AbstractFeature"/>
  <xs:complexType name="UnderscoreFeat_Type">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="desc" type="xs:string"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>

  <!-- CanVec prefixes XyZz, XyZ1, XyZ2 are skipped -->
  <xs:element name="XyZzIgnored" type="XyZzIgnoredType" substitutionGroup="gml:_Feature"/>
  <xs:element name="XyZ1Ignored" type="XyZ1IgnoredType" substitutionGroup="gml:_Feature"/>
  <xs:element name="XyZ2Ignored" type="XyZ2IgnoredType" substitutionGroup="gml:_Feature"/>

  <!-- Element with inline complexType -->
  <xs:element name="InlineFeat" substitutionGroup="gml:AbstractFeature">
    <xs:complexType>
      <xs:complexContent>
        <xs:extension base="gml:AbstractFeatureType">
          <xs:sequence>
            <xs:element name="field1" type="xs:string"/>
          </xs:sequence>
        </xs:extension>
      </xs:complexContent>
    </xs:complexType>
  </xs:element>

</xs:schema>
)xml";

  VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/naming.xsd", kXml);
  std::vector<GMLFeatureClass*> classes;
  bool fully_understood = false;
  EXPECT_TRUE(GMLParseXSD("/vsimem/parsexsd_test/naming.xsd", false, classes,
                          fully_understood));
  absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
  EXPECT_TRUE(fully_understood);
  ASSERT_EQ(5, classes.size());

  EXPECT_STREQ("Air", classes[0]->GetName());
  EXPECT_STREQ("RekisteriyksikonPalstanTietoja", classes[1]->GetName());
  EXPECT_STREQ("TargetFeature", classes[2]->GetName());
  EXPECT_STREQ("UnderscoreFeat", classes[3]->GetName());
  EXPECT_STREQ("InlineFeat", classes[4]->GetName());

  // Check that the unique constraint from FeatureCollection was attached to
  // TargetFeature.unique_id.
  GMLPropertyDefn* prop_uid = classes[2]->GetProperty("unique_id");
  ASSERT_NE(nullptr, prop_uid);
  EXPECT_TRUE(prop_uid->IsUnique());

  GMLPropertyDefn* prop_norm = classes[2]->GetProperty("normal_field");
  ASSERT_NE(nullptr, prop_norm);
  EXPECT_FALSE(prop_norm->IsUnique());
}

TEST(ParseXsdTest, UnsupportedConstructsAndAborts) {
  // 1. Group element causes abort.
  {
    constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="GroupFeat" type="GroupFeatType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="GroupFeatType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:group ref="SomeGroup"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/group.xsd", kXml);
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/group.xsd", false, classes,
                             fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_FALSE(fully_understood);
  }

  // 2. Choice with unsupported ref causes abort.
  {
    constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="ChoiceFeat" type="ChoiceFeatType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="ChoiceFeatType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:choice>
            <xs:element ref="gml:unsupportedRef"/>
          </xs:choice>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/choice.xsd", kXml);
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/choice.xsd", false, classes,
                             fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_FALSE(fully_understood);
  }

  // 3. Unknown gml type causes abort.
  {
    constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="BadGmlFeat" type="BadGmlFeatType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="BadGmlFeatType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="g" type="gml:UnknownNonGeomType"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/bad_gml.xsd", kXml);
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/bad_gml.xsd", false,
                             classes, fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_FALSE(fully_understood);
  }

  // 4. Unknown simpleType name causes abort.
  {
    constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="BadSimpleFeat" type="BadSimpleFeatType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="BadSimpleFeatType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="f" type="unknownSimpleType"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/bad_simple.xsd", kXml);
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/bad_simple.xsd", false,
                             classes, fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_FALSE(fully_understood);
  }

  // 5. Unknown ref="gml:..." causes abort.
  {
    constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="BadRefFeat" type="BadRefFeatType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="BadRefFeatType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element ref="gml:UnknownRefType"/>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/bad_ref.xsd", kXml);
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/bad_ref.xsd", false,
                             classes, fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_FALSE(fully_understood);
  }

  // 6. Complex element without simpleType or gml:_Geometry causes abort.
  {
    constexpr const char kXml[] = R"xml(
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
           xmlns:gml="http://www.opengis.net/gml">
  <xs:element name="BadComplexFeat" type="BadComplexFeatType" substitutionGroup="gml:_Feature"/>
  <xs:complexType name="BadComplexFeatType">
    <xs:complexContent>
      <xs:extension base="gml:AbstractFeatureType">
        <xs:sequence>
          <xs:element name="nested_elem">
            <xs:complexType>
              <xs:sequence>
                <xs:element name="leaf" type="xs:string"/>
              </xs:sequence>
            </xs:complexType>
          </xs:element>
        </xs:sequence>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>
)xml";
    VsiMemTempWrapper wrapper("/vsimem/parsexsd_test/bad_complex.xsd", kXml);
    std::vector<GMLFeatureClass*> classes;
    bool fully_understood = false;
    EXPECT_FALSE(GMLParseXSD("/vsimem/parsexsd_test/bad_complex.xsd", false,
                             classes, fully_understood));
    absl::Cleanup cleaner = [&classes] { CleanClasses(classes); };
    EXPECT_FALSE(fully_understood);
  }
}

// TODO: b/335317901 - GMLParseXSD achieves 92.42% line coverage. Track
// gmgml:Polygon_Surface_MultiSurface_CompositeSurfacePropertyType and
// ogr:m_coordinate_resolution.

}  // namespace
}  // namespace autotest2
