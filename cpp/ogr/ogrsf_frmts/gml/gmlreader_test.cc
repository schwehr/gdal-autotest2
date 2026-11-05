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
// Tests for GMLReader (gmlreader.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/gmlreader.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreader.h"

#include <climits>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreaderp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(GMLReaderTest, SetupParserFailures) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  WithQuietHandler quiet_handler;
  reader->SetSourceFile("/vsimem/gmlreader_test/nonexistent.gml");
  EXPECT_EQ(nullptr, reader->NextFeature());
}

TEST(GMLReaderTest, ClassManagementAndStateStack) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  EXPECT_EQ(0, reader->GetClassCount());
  EXPECT_EQ(nullptr, reader->GetClass(0));
  EXPECT_EQ(nullptr, reader->GetClass("Missing"));

  auto* class_a = new GMLFeatureClass("LayerA");
  EXPECT_EQ(0, reader->AddClass(class_a));
  EXPECT_EQ(1, reader->GetClassCount());
  EXPECT_EQ(class_a, reader->GetClass(0));
  EXPECT_EQ(class_a, reader->GetClass("LayerA"));
  EXPECT_EQ(nullptr, reader->GetClass(1));

  // PushState and PopState recycling.
  auto* state1 = new GMLReadState();
  state1->PushPath("root");
  reader->PushState(state1);
  EXPECT_EQ(state1, reader->GetState());

  reader->PopState();
  EXPECT_EQ(nullptr, reader->GetState());

  auto* state2 = new GMLReadState();
  reader->PushState(state2);
  EXPECT_EQ(state2, reader->GetState());
  reader->PopState();

  // ClearClasses deletes classes and resets class count.
  reader->ClearClasses();
  EXPECT_EQ(0, reader->GetClassCount());
}

TEST(GMLReaderTest, FeatureElementIndexVariants) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  auto* state = new GMLReadState();
  reader->PushState(state);

  // 1. APPSCHEMA_MTKGML: requires path length == 1.
  state->Reset();
  state->PushPath("root");
  state->PushPath("container");
  EXPECT_EQ(-1, reader->GetFeatureElementIndex("any", 3, APPSCHEMA_MTKGML));

  state->Reset();
  state->PushPath("Maastotiedot");
  EXPECT_EQ(INT_MAX,
            reader->GetFeatureElementIndex("leaf", 4, APPSCHEMA_MTKGML));

  // 2. member / members.
  state->Reset();
  state->PushPath("featureMember");
  EXPECT_EQ(INT_MAX,
            reader->GetFeatureElementIndex("Feature", 7, APPSCHEMA_GENERIC));

  state->Reset();
  state->PushPath("featureMembers");
  EXPECT_EQ(INT_MAX,
            reader->GetFeatureElementIndex("Feature", 7, APPSCHEMA_GENERIC));

  // 3. Polish TBD GML (dane).
  state->Reset();
  state->PushPath("dane");
  EXPECT_EQ(INT_MAX,
            reader->GetFeatureElementIndex("Objekt", 6, APPSCHEMA_GENERIC));

  // 4. OpenLS.
  state->Reset();
  state->PushPath("GeocodeResponseList");
  EXPECT_EQ(INT_MAX, reader->GetFeatureElementIndex("GeocodedAddress", 15,
                                                    APPSCHEMA_GENERIC));

  state->Reset();
  state->PushPath("DetermineRouteResponse");
  EXPECT_EQ(-1, reader->GetFeatureElementIndex("RouteInstructionsList", 21,
                                               APPSCHEMA_GENERIC));

  state->Reset();
  state->PushPath("RouteInstructionsList");
  EXPECT_EQ(INT_MAX, reader->GetFeatureElementIndex("RouteInstruction", 16,
                                                    APPSCHEMA_GENERIC));

  // 5. MapServer WMS.
  state->Reset();
  state->PushPath("roads_layer");
  EXPECT_EQ(INT_MAX, reader->GetFeatureElementIndex("roads_feature", 13,
                                                    APPSCHEMA_GENERIC));

  // 6. CSW SearchResults.
  state->Reset();
  state->PushPath("SearchResults");
  EXPECT_EQ(INT_MAX, reader->GetFeatureElementIndex("BriefRecord", 11,
                                                    APPSCHEMA_GENERIC));
  EXPECT_EQ(INT_MAX, reader->GetFeatureElementIndex("SummaryRecord", 13,
                                                    APPSCHEMA_GENERIC));
  EXPECT_EQ(INT_MAX,
            reader->GetFeatureElementIndex("Record", 6, APPSCHEMA_GENERIC));

  // 7. Locked class list: matching by name and nested element paths.
  reader->ClearClasses();
  auto* class1 = new GMLFeatureClass("Roads");
  reader->AddClass(class1);

  auto* class2 = new GMLFeatureClass("Parcel");
  class2->SetElementName("Cadastre|Parcel");
  reader->AddClass(class2);

  reader->SetClassListLocked(true);

  // Normal element matching.
  state->Reset();
  state->PushPath("featureMember");
  EXPECT_EQ(0, reader->GetFeatureElementIndex("Roads", 5, APPSCHEMA_GENERIC));
  EXPECT_EQ(-1,
            reader->GetFeatureElementIndex("Unknown", 7, APPSCHEMA_GENERIC));

  // Nested element matching.
  state->Reset();
  state->PushPath("Cadastre");
  EXPECT_EQ(1, reader->GetFeatureElementIndex("Parcel", 6, APPSCHEMA_GENERIC));

  // Reset before teardown.
  reader->PopState();
}

TEST(GMLReaderTest, GetAttributeElementIndexVariants) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  auto* poClass = new GMLFeatureClass("Building");
  reader->AddClass(poClass);

  // Push feature so m_poState->m_poFeature is non-null.
  reader->PushFeature("Building", "id.1", 0);
  absl::Cleanup feature_cleaner = [&reader] {
    delete reader->GetState()->m_poFeature;
    reader->GetState()->m_poFeature = nullptr;
    reader->PopState();
  };

  // Unlocked schema returns INT_MAX.
  EXPECT_EQ(INT_MAX, reader->GetAttributeElementIndex("roofType", 8));

  // Lock schema and add properties.
  poClass->SetSchemaLocked(true);
  poClass->AddProperty(new GMLPropertyDefn("roofType", "roofType"));
  poClass->AddProperty(new GMLPropertyDefn("roofType_uom", "roofType@uom"));
  poClass->AddProperty(new GMLPropertyDefn("nested_prop", "nested|prop"));
  poClass->AddProperty(
      new GMLPropertyDefn("nested_prop_uom", "nested|prop@uom"));

  // Path length 0.
  EXPECT_EQ(0, reader->GetAttributeElementIndex("roofType", 8, nullptr));
  EXPECT_EQ(1, reader->GetAttributeElementIndex("roofType", 8, "uom"));
  EXPECT_EQ(-1, reader->GetAttributeElementIndex("unknown", 7, nullptr));

  // Path length > 0.
  reader->GetState()->PushPath("nested");
  EXPECT_EQ(2, reader->GetAttributeElementIndex("prop", 4, nullptr));
  EXPECT_EQ(3, reader->GetAttributeElementIndex("prop", 4, "uom"));
  EXPECT_EQ(-1, reader->GetAttributeElementIndex("other", 5, nullptr));
}

TEST(GMLReaderTest, SetFeaturePropertyDirectlyLaunderingAndTypes) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  auto* poClass = new GMLFeatureClass("TestClass");
  reader->AddClass(poClass);

  // Push a feature.
  reader->PushFeature("TestClass", "fid.1", 0);
  absl::Cleanup feature_cleaner = [&reader] {
    delete reader->GetState()->m_poFeature;
    reader->GetState()->m_poFeature = nullptr;
    reader->PopState();
  };

  // Property name with special characters '@' and '|'.
  reader->SetFeaturePropertyDirectly("attr@code", CPLStrdup("val1"), -1);
  EXPECT_EQ(1, poClass->GetPropertyCount());
  EXPECT_STREQ("attr_code", poClass->GetProperty(0)->GetName());

  // Typed property GMLPT_Real.
  reader->SetFeaturePropertyDirectly("price", CPLStrdup("12.34"), -1,
                                     GMLPT_Real);
  EXPECT_EQ(2, poClass->GetPropertyCount());
  EXPECT_EQ(GMLPT_Real, poClass->GetProperty(1)->GetType());

  // Test conflicting property name resolution (>10 collisions).
  for (int i = 0; i < 12; ++i) {
    std::string name = "field";
    for (int j = 0; j < i; ++j) {
      name += '_';
    }
    poClass->AddProperty(new GMLPropertyDefn(name.c_str(), name.c_str()));
  }
  {
    WithQuietHandler quiet_handler;
    reader->SetFeaturePropertyDirectly("field", CPLStrdup("collision"), -1);
  }

  // Locked schema ignores unknown property.
  reader->SetClassListLocked(true);
  {
    WithQuietHandler quiet_handler;
    reader->SetFeaturePropertyDirectly("unknown", CPLStrdup("val"), -1);
  }

  // Width flag with AnalysePropertyValue.
  reader->SetWidthFlag(true);
  reader->SetFeaturePropertyDirectly("price", CPLStrdup("9999.99"), 1);
}

TEST(GMLReaderTest, GlobalSRSNameAndFilteredClass) {
  // 1. Compound SRS "EPSG:4326, EPSG:5773".
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetGlobalSRSName("EPSG:4326, EPSG:5773");
    EXPECT_TRUE(reader->CanUseGlobalSRSName());
    EXPECT_STREQ("EPSG:4326+5773", reader->GetGlobalSRSName());
  }

  // 2. ConsiderEPSGAsURN SRS.
  {
    auto reader = std::make_unique<GMLReader>(
        true, false, /*bConsiderEPSGAsURN=*/true, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetGlobalSRSName("EPSG:4326");
    EXPECT_TRUE(reader->CanUseGlobalSRSName());
    EXPECT_STREQ("urn:ogc:def:crs:EPSG::4326", reader->GetGlobalSRSName());
  }

  // 3. Normal SRS and Filtered class name.
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetGlobalSRSName("EPSG:32632");
    EXPECT_STREQ("EPSG:32632", reader->GetGlobalSRSName());

    auto* class_a = new GMLFeatureClass("LayerA");
    auto* class_b = new GMLFeatureClass("LayerB");
    reader->AddClass(class_a);
    reader->AddClass(class_b);

    EXPECT_TRUE(reader->SetFilteredClassName("LayerA"));
    EXPECT_STREQ("LayerA", reader->GetFilteredClassName());

    reader->ResetReading();
    EXPECT_EQ(nullptr, reader->GetFilteredClassName());
  }
}

TEST(GMLReaderTest, WFSJointLayer) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  EXPECT_FALSE(reader->IsWFSJointLayer());
}

TEST(GMLReaderTest, LoadAndSaveClasses) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  // LoadClasses failure paths.
  EXPECT_FALSE(reader->LoadClasses(nullptr));
  EXPECT_FALSE(reader->LoadClasses("/vsimem/gmlreader_test/nonexistent.gfs"));

  constexpr std::string_view kInvalidGfs = "<not_gml_feature_class_list/>";
  constexpr char kInvalidGfsPath[] = "/vsimem/gmlreader_test/invalid.gfs";
  VsiMemTempWrapper file_invalid(kInvalidGfsPath, std::string(kInvalidGfs));
  EXPECT_FALSE(reader->LoadClasses(kInvalidGfsPath));

  constexpr std::string_view kValidGfs = R"xml(
<GMLFeatureClassList>
  <SequentialLayers>true</SequentialLayers>
  <GMLFeatureClass>
    <Name>Buildings</Name>
    <ElementPath>Buildings</ElementPath>
    <GeometryType>3</GeometryType>
    <SRSName>EPSG:4326</SRSName>
    <DatasetSpecificInfo>
      <FeatureCount>10</FeatureCount>
      <ExtentXMin>10.0</ExtentXMin>
      <ExtentXMax>20.0</ExtentXMax>
      <ExtentYMin>30.0</ExtentYMin>
      <ExtentYMax>40.0</ExtentYMax>
    </DatasetSpecificInfo>
    <PropertyDefn>
      <Name>height</Name>
      <ElementPath>height</ElementPath>
      <Type>Real</Type>
      <Width>10</Width>
      <Precision>2</Precision>
    </PropertyDefn>
    <PropertyDefn>
      <Name>owner</Name>
      <ElementPath>owner</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
  </GMLFeatureClass>
  <GMLFeatureClass>
    <Name>Roads</Name>
    <ElementPath>Roads</ElementPath>
    <GeometryType>2</GeometryType>
  </GMLFeatureClass>
</GMLFeatureClassList>
)xml";
  constexpr char kValidGfsPath[] = "/vsimem/gmlreader_test/valid.gfs";
  VsiMemTempWrapper file_valid(kValidGfsPath, std::string(kValidGfs));

  EXPECT_TRUE(reader->LoadClasses(kValidGfsPath));
  EXPECT_EQ(2, reader->GetClassCount());
  EXPECT_STREQ("Buildings", reader->GetClass(0)->GetName());
  EXPECT_EQ(2, reader->GetClass(0)->GetPropertyCount());
  EXPECT_EQ(10, reader->GetClass(0)->GetFeatureCount());

  // SaveClasses.
  constexpr char kSaveGfsPath[] = "/vsimem/gmlreader_test/saved.gfs";
  EXPECT_TRUE(reader->SaveClasses(kSaveGfsPath));

  // Reload saved GFS and verify.
  auto reader_reload =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader_reload);
  EXPECT_TRUE(reader_reload->LoadClasses(kSaveGfsPath));
  EXPECT_EQ(2, reader_reload->GetClassCount());
  VSIUnlink(kSaveGfsPath);

  // SaveClasses unwritable fails.
  WithQuietHandler quiet_handler;
  EXPECT_FALSE(reader->SaveClasses("/proc/unwritable_dir/saved.gfs"));
  EXPECT_FALSE(reader->SaveClasses(nullptr));
}

constexpr std::string_view kPrescanMultiGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy>
    <gml:Box srsName="EPSG:4326">
      <gml:coordinates>10,20 30,40</gml:coordinates>
    </gml:Box>
  </gml:boundedBy>
  <gml:featureMember>
    <Place>
      <name>Alpha</name>
      <gml:Point srsName="EPSG:4326">
        <gml:coordinates>15,25</gml:coordinates>
      </gml:Point>
    </Place>
  </gml:featureMember>
  <gml:featureMember>
    <Place>
      <name>Beta</name>
      <gml:Point srsName="EPSG:4326">
        <gml:coordinates>20,30</gml:coordinates>
      </gml:Point>
    </Place>
  </gml:featureMember>
  <gml:featureMember>
    <Road>
      <name>Highway</name>
      <gml:LineString srsName="EPSG:4326">
        <gml:coordinates>10,20 20,30 30,40</gml:coordinates>
      </gml:LineString>
    </Road>
  </gml:featureMember>
</root>
)xml";

TEST(GMLReaderTest, PrescanForSchemaExtentsAndSequentialLayers) {
  constexpr char kPath[] = "/vsimem/gmlreader_test/prescan_multi.gml";
  VsiMemTempWrapper file(kPath, std::string(kPrescanMultiGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  // Null source file fails.
  {
    auto null_reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(null_reader->PrescanForSchema(true, false));
  }

  // Prescan with bGetExtents=true and bOnlyDetectSRS=false.
  EXPECT_TRUE(
      reader->PrescanForSchema(/*bGetExtents=*/true, /*bOnlyDetectSRS=*/false));
  EXPECT_EQ(2, reader->GetClassCount());

  GMLFeatureClass* poPlace = reader->GetClass(0);
  ASSERT_NE(nullptr, poPlace);
  EXPECT_STREQ("Place", poPlace->GetName());
  EXPECT_EQ(2, poPlace->GetFeatureCount());
  EXPECT_TRUE(poPlace->HasExtents());

  GMLFeatureClass* poRoad = reader->GetClass(1);
  ASSERT_NE(nullptr, poRoad);
  EXPECT_STREQ("Road", poRoad->GetName());
  EXPECT_EQ(1, poRoad->GetFeatureCount());

  // Test NextFeature iteration across sequential layers.
  reader->ResetReading();
  int count = 0;
  while (std::unique_ptr<GMLFeature> feat{reader->NextFeature()}) {
    count++;
  }
  EXPECT_EQ(3, count);
}

constexpr std::string_view kCityGmlSample = R"xml(
<CityModel xmlns="http://www.opengis.net/citygml/2.0"
           xmlns:gml="http://www.opengis.net/gml">
  <cityObjectMember>
    <Building gml:id="bld1">
      <stringAttribute name="roof_style">
        <value>gable</value>
      </stringAttribute>
      <intAttribute name="stories">
        <value>3</value>
      </intAttribute>
      <doubleAttribute name="height">
        <value>12.5</value>
      </doubleAttribute>
      <gml:Point srsName="EPSG:4326">
        <gml:coordinates>10.0,20.0</gml:coordinates>
      </gml:Point>
    </Building>
  </cityObjectMember>
</CityModel>
)xml";

TEST(GMLReaderTest, CityGMLGenericAttributesParsing) {
  constexpr char kPath[] = "/vsimem/gmlreader_test/citygml.gml";
  VsiMemTempWrapper file(kPath, std::string(kCityGmlSample));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(
      reader->PrescanForSchema(/*bGetExtents=*/true, /*bOnlyDetectSRS=*/false));
  EXPECT_EQ(1, reader->GetClassCount());

  GMLFeatureClass* poClass = reader->GetClass(0);
  ASSERT_NE(nullptr, poClass);
  EXPECT_STREQ("Building", poClass->GetName());

  reader->ResetReading();
  std::unique_ptr<GMLFeature> feat(reader->NextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("bld1", feat->GetFID());
  while (std::unique_ptr<GMLFeature> next_feat{reader->NextFeature()}) {
  }
}

constexpr std::string_view kInterleavedGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember><LayerA><name>A1</name></LayerA></gml:featureMember>
  <gml:featureMember><LayerB><name>B1</name></LayerB></gml:featureMember>
  <gml:featureMember><LayerA><name>A2</name></LayerA></gml:featureMember>
</root>
)xml";

TEST(GMLReaderTest, PrescanInterleavedLayersAndOnlyDetectSRS) {
  constexpr char kPath[] = "/vsimem/gmlreader_test/interleaved.gml";
  VsiMemTempWrapper file(kPath, std::string(kInterleavedGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  // bOnlyDetectSRS=true path.
  EXPECT_TRUE(reader->PrescanForSchema(/*bGetExtents=*/false,
                                       /*bOnlyDetectSRS=*/true));

  // bGetExtents=false path.
  EXPECT_TRUE(reader->PrescanForSchema(/*bGetExtents=*/false,
                                       /*bOnlyDetectSRS=*/false));
  EXPECT_EQ(2, reader->GetClassCount());
}

TEST(GMLReaderTest, CreateGMLReaderAndSetFP) {
  std::unique_ptr<IGMLReader> ireader(
      CreateGMLReader(true, false, false, GML_SWAP_AUTO, false));
  ASSERT_NE(nullptr, ireader);

  GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
  EXPECT_EQ(nullptr, reader.GetSourceFileName());

  constexpr char kPath[] = "/vsimem/gmlreader_test/dummy.gml";
  reader.SetSourceFile(kPath);
  EXPECT_STREQ(kPath, reader.GetSourceFileName());

  reader.SetFP(nullptr);
}

TEST(GMLReaderTest, TrailingNulAndXmlParseErrors) {
  // 1. Valid feature followed by invalid XML with trailing NUL bytes.
  std::string s =
      "<root xmlns:gml=\"http://www.opengis.net/gml\">"
      "<gml:featureMember><F><n>1</n></F></gml:featureMember><broken";
  s.push_back('\0');
  s.push_back('\0');

  constexpr char kPath1[] = "/vsimem/gmlreader_test/trailing_nul.gml";
  VsiMemTempWrapper file1(kPath1, s);

  {
    GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
    reader.SetSourceFile(kPath1);
    std::unique_ptr<GMLFeature> f1(reader.NextFeature());
    ASSERT_NE(nullptr, f1);

    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, reader.NextFeature());
  }

  // 2. Broken XML only.
  constexpr char kPath2[] = "/vsimem/gmlreader_test/broken.gml";
  VsiMemTempWrapper file2(kPath2, "<root><broken");
  {
    GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
    reader.SetSourceFile(kPath2);
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, reader.NextFeature());
  }
}

TEST(GMLReaderTest, DetermineRouteResponseAndAddClassFeatureProperty) {
  GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
  auto* state = new GMLReadState();
  state->PushPath("DetermineRouteResponse");
  reader.PushState(state);
  EXPECT_EQ(INT_MAX,
            reader.GetFeatureElementIndex("OtherElem", 9, APPSCHEMA_GENERIC));
  reader.PopState();

  auto* fc = new GMLFeatureClass("WithFP");
  auto* p = new GMLPropertyDefn("fp", "fp");
  p->SetType(GMLPT_FeatureProperty);
  fc->AddProperty(p);
  EXPECT_EQ(0, reader.AddClass(fc));
}

TEST(GMLReaderTest, WFSJointLayerAndPropertyBranches) {
  // 1. WFS Joint Layer property naming.
  {
    GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
    reader.SetIsWFSJointLayer(true);
    auto* fc = new GMLFeatureClass("_ogr_wfs_joint_layer_");
    reader.AddClass(fc);
    EXPECT_TRUE(reader.IsWFSJointLayer());

    reader.PushFeature("_ogr_wfs_joint_layer_", "fid1", 0);
    absl::Cleanup feature_cleaner = [&reader] {
      delete reader.GetState()->m_poFeature;
      reader.GetState()->m_poFeature = nullptr;
      reader.PopState();
    };
    reader.SetFeaturePropertyDirectly("member|layer|prop", CPLStrdup("val1"),
                                      -1);
    reader.SetFeaturePropertyDirectly("member|layer@id", CPLStrdup("id1"), -1);

    EXPECT_NE(nullptr, fc->GetProperty("layer.prop"));
    EXPECT_NE(nullptr, fc->GetProperty("layer.gml_id"));
  }

  // 2. Nested property name with '|' collision fallback.
  {
    GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
    auto* fc = new GMLFeatureClass("Class2");
    reader.AddClass(fc);
    reader.PushFeature("Class2", "fid2", 0);
    absl::Cleanup feature_cleaner = [&reader] {
      delete reader.GetState()->m_poFeature;
      reader.GetState()->m_poFeature = nullptr;
      reader.PopState();
    };

    reader.SetFeaturePropertyDirectly("parent1|child", CPLStrdup("v1"), -1);
    EXPECT_NE(nullptr, fc->GetProperty("child"));

    reader.SetFeaturePropertyDirectly("parent2|child", CPLStrdup("v2"), -1);
    EXPECT_NE(nullptr, fc->GetProperty("parent2|child"));
  }

  // 3. Name collision loop (>10 collisions) and GML_FIELDTYPES=ALWAYS_STRING.
  {
    GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
    auto* fc = new GMLFeatureClass("Class3");
    reader.AddClass(fc);
    reader.PushFeature("Class3", "fid3", 0);
    absl::Cleanup feature_cleaner = [&reader] {
      delete reader.GetState()->m_poFeature;
      reader.GetState()->m_poFeature = nullptr;
      reader.PopState();
    };

    for (int i = 0; i <= 10; ++i) {
      std::string name = "dup";
      for (int j = 0; j < i; ++j) {
        name += "_";
      }
      fc->AddProperty(new GMLPropertyDefn(name.c_str(), "other_src"));
    }

    CPLSetConfigOption("GML_FIELDTYPES", "ALWAYS_STRING");
    absl::Cleanup reset_ft = [] {
      CPLSetConfigOption("GML_FIELDTYPES", nullptr);
    };

    reader.SetFeaturePropertyDirectly("new_str", CPLStrdup("hello"), -1);
    EXPECT_EQ(GMLPT_String, fc->GetProperty("new_str")->GetType());

    {
      WithQuietHandler quiet_handler;
      reader.SetFeaturePropertyDirectly("dup", CPLStrdup("overflow"), -1);
    }
  }

  // 4. Class schema locked ignores unknown property.
  {
    GMLReader reader(true, false, false, GML_SWAP_AUTO, false);
    auto* fc = new GMLFeatureClass("Class4");
    fc->SetSchemaLocked(true);
    reader.AddClass(fc);
    reader.PushFeature("Class4", "fid4", 0);
    absl::Cleanup feature_cleaner = [&reader] {
      delete reader.GetState()->m_poFeature;
      reader.GetState()->m_poFeature = nullptr;
      reader.PopState();
    };

    reader.SetFeaturePropertyDirectly("missing", CPLStrdup("val"), -1);
    EXPECT_EQ(0, fc->GetPropertyCount());
  }
}

TEST(GMLReaderTest, LoadClassesErrorBranches) {
  GMLReader reader(true, false, false, GML_SWAP_AUTO, false);

  // 1. Unclosed XML.
  constexpr char kErr1[] = "/vsimem/gmlreader_test/err1.gfs";
  VsiMemTempWrapper f1(kErr1, "<!-- <GMLFeatureClassList --> <unclosed");
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(reader.LoadClasses(kErr1));
  }

  // 2. Wrong root element.
  constexpr char kErr2[] = "/vsimem/gmlreader_test/err2.gfs";
  VsiMemTempWrapper f2(kErr2, "<!-- <GMLFeatureClassList --> <WrongRoot/>");
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(reader.LoadClasses(kErr2));
  }

  // 3. Failed class initialization.
  constexpr char kErr3[] = "/vsimem/gmlreader_test/err3.gfs";
  VsiMemTempWrapper f3(
      kErr3,
      "<GMLFeatureClassList><GMLFeatureClass><NoName/></GMLFeatureClass></"
      "GMLFeatureClassList>");
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(reader.LoadClasses(kErr3));
  }
}

constexpr std::string_view kBoundedByFallbackGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <Area>
      <name>TestArea</name>
      <gml:Point srsName="EPSG:4326">
        <gml:coordinates>10,20</gml:coordinates>
      </gml:Point>
    </Area>
  </gml:featureMember>
</root>
)xml";

constexpr std::string_view kNestedGeomGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <P1>
      <container_geom>
        <gml:Point srsName="EPSG:32632">
          <gml:coordinates>500000,4000000</gml:coordinates>
        </gml:Point>
      </container_geom>
    </P1>
  </gml:featureMember>
</root>
)xml";

TEST(GMLReaderTest, PrescanForSchemaSRSAndBoundedByFallback) {
  // 1. boundedBy fallback with lat-long extent swap.
  constexpr char kPath1[] = "/vsimem/gmlreader_test/bounded_by.gml";
  VsiMemTempWrapper f1(kPath1, std::string(kBoundedByFallbackGml));

  {
    GMLReader reader(/*bExpatReader=*/true,
                     /*bInvertAxisOrderIfLatLong=*/true,
                     /*bConsiderEPSGAsURN=*/false, GML_SWAP_AUTO, false);
    reader.SetSourceFile(kPath1);
    reader.SetGlobalSRSName("EPSG:4326");

    EXPECT_TRUE(reader.PrescanForSchema(true, false));
    EXPECT_EQ(1, reader.GetClassCount());
    EXPECT_TRUE(reader.GetClass(0)->HasExtents());

    // Subsequent Prescan with bOnlyDetectSRS=true on existing classes.
    EXPECT_TRUE(reader.PrescanForSchema(true, /*bOnlyDetectSRS=*/true));
  }

  // 2. Nested geometry name and per-feature SRS mismatch.
  constexpr char kPath2[] = "/vsimem/gmlreader_test/nested_geom.gml";
  VsiMemTempWrapper f2(kPath2, std::string(kNestedGeomGml));

  {
    GMLReader reader(/*bExpatReader=*/true, false, false, GML_SWAP_AUTO, false);
    reader.SetSourceFile(kPath2);
    reader.SetGlobalSRSName("EPSG:4326");

    EXPECT_TRUE(reader.PrescanForSchema(true, false));
    EXPECT_EQ(1, reader.GetClassCount());
    EXPECT_FALSE(reader.CanUseGlobalSRSName());
  }
}

}  // namespace
}  // namespace autotest2
