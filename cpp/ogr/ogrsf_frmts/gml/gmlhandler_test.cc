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
// Tests for GMLHandler & GMLExpatHandler (gmlhandler.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/gmlhandler.cpp

#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreaderp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlutils.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

TEST(GMLHandlerTest, OGRGMLIsGeometryElementCheck) {
  EXPECT_TRUE(OGRGMLIsGeometryElement("Point"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("LineString"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Polygon"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("MultiPoint"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("MultiLineString"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("MultiPolygon"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("BoundingBox"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("CompositeCurve"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("CompositeSurface"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Shell"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Curve"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Surface"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Solid"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Tin"));
  EXPECT_TRUE(OGRGMLIsGeometryElement("Triangle"));
  EXPECT_FALSE(OGRGMLIsGeometryElement("NotAGeom"));
  EXPECT_FALSE(OGRGMLIsGeometryElement("featureMember"));
}

constexpr std::string_view kAixmSample = R"xml(
<AIXMBasicMessage xmlns:gml="http://www.opengis.net/gml">
  <hasMember>
    <AirportHeliport gml:id="ah1">
      <timeSlice>
        <ElevatedPoint srsName="EPSG:4326">
          <gml:pos>60.0 24.0</gml:pos>
          <elevation uom="FT">123.4</elevation>
          <geoidUndulation uom="M">5.6</geoidUndulation>
          <verticalDatum>EGM96</verticalDatum>
          <verticalAccuracy uom="FT">10</verticalAccuracy>
        </ElevatedPoint>
        <ElevatedPoint srsName="EPSG:4326">
          <elevation uom="FT">0.0</elevation>
        </ElevatedPoint>
        <ElevatedCurve srsName="EPSG:4326">
          <gml:posList>60.0 24.0 60.1 24.1</gml:posList>
          <elevation uom="M">50</elevation>
        </ElevatedCurve>
        <ElevatedSurface srsName="EPSG:4326">
          <gml:posList>60.0 24.0 60.1 24.0 60.1 24.1 60.0 24.0</gml:posList>
          <elevation uom="M">10</elevation>
        </ElevatedSurface>
      </timeSlice>
    </AirportHeliport>
  </hasMember>
  <hasMember>
    <RouteSegment gml:id="rs1">
      <start>
        <gml:Point><gml:pos>60.0 24.0</gml:pos></gml:Point>
      </start>
      <Curve>
        <gml:segments>
          <gml:LineStringSegment>
            <gml:posList>60.0 24.0 60.5 24.5</gml:posList>
          </gml:LineStringSegment>
        </gml:segments>
      </Curve>
    </RouteSegment>
  </hasMember>
  <hasMember>
    <horizontalProjection_Runway gml:id="hp1">
      <horizontalProjection_location>
        <gml:Point><gml:pos>60.0 24.0</gml:pos></gml:Point>
      </horizontalProjection_location>
      <otherGeom>
        <gml:Point><gml:pos>60.1 24.1</gml:pos></gml:Point>
      </otherGeom>
    </horizontalProjection_Runway>
  </hasMember>
</AIXMBasicMessage>
)xml";

TEST(GMLHandlerTest, AixmSchemaAndElevatedGeometries) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/aixm.gml";
  VsiMemTempWrapper file(kPath, std::string(kAixmSample));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  EXPECT_EQ(3, reader->GetClassCount());

  reader->ResetReading();
  std::unique_ptr<GMLFeature> f1(reader->NextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_STREQ("ah1", f1->GetFID());

  std::unique_ptr<GMLFeature> f2(reader->NextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_STREQ("rs1", f2->GetFID());

  std::unique_ptr<GMLFeature> f3(reader->NextFeature());
  ASSERT_NE(nullptr, f3);
  EXPECT_STREQ("hp1", f3->GetFID());

  EXPECT_EQ(nullptr, reader->NextFeature());
}

constexpr std::string_view kMtkgmlSample = R"xml(
<Maastotiedot srsName="EPSG:3067" xmlns:gml="http://www.opengis.net/gml">
  <sijainti>
    <Tieviiva gid="1001">
      <Piste srsName="EPSG:3067">
        <gml:coordinates>385000,6675000</gml:coordinates>
      </Piste>
      <teksti kieli="fin">Helsinki</teksti>
    </Tieviiva>
    <Tieviiva gid="1002">
      <Murtoviiva srsName="EPSG:3067">
        <gml:coordinates>385000,6675000 386000,6676000</gml:coordinates>
      </Murtoviiva>
    </Tieviiva>
    <Tieviiva gid="1003">
      <Alue srsName="EPSG:3067">
        <ulkoreuna>
          <gml:coordinates>385000,6675000 386000,6675000 386000,6676000 385000,6675000</gml:coordinates>
        </ulkoreuna>
      </Alue>
    </Tieviiva>
  </sijainti>
</Maastotiedot>
)xml";

TEST(GMLHandlerTest, MtkgmlSchemaAndCustomGeometries) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/mtkgml.gml";
  VsiMemTempWrapper file(kPath, std::string(kMtkgmlSample));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  EXPECT_EQ(1, reader->GetClassCount());

  reader->ResetReading();
  int count = 0;
  while (std::unique_ptr<GMLFeature> feat{reader->NextFeature()}) {
    count++;
  }
  EXPECT_EQ(3, count);
}

constexpr std::string_view kInternalXlinkGeomGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <gml:featureMember>
    <Feature gml:id="f1">
      <geom>
        <gml:MultiCurve>
          <gml:curveMember xlink:href="#ls1"/>
          <gml:curveMember>
            <gml:LineString gml:id="ls1"><gml:posList>0 0 1 1</gml:posList></gml:LineString>
          </gml:curveMember>
          <gml:curveMember xlink:href="#ls_cycle">
            <gml:LineString gml:id="ls_cycle"><gml:posList>2 2 3 3</gml:posList></gml:LineString>
          </gml:curveMember>
        </gml:MultiCurve>
      </geom>
      <prop>Done</prop>
    </Feature>
  </gml:featureMember>
</root>
)xml";

TEST(GMLHandlerTest, InternalGeometryXlinksAndCycleDetection) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/xlink_geom.gml";
  VsiMemTempWrapper file(kPath, std::string(kInternalXlinkGeomGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  reader->ResetReading();

  std::unique_ptr<GMLFeature> f1(reader->NextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_STREQ("f1", f1->GetFID());

  EXPECT_EQ(nullptr, reader->NextFeature());
}

constexpr std::string_view kConditionsGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml" xmlns:xlink="http://www.w3.org/1999/xlink">
  <gml:featureMember>
    <Item>
      <val class="A" flag="1" code="ok" id="id1">10</val>
      <bad attr="v">err1</bad>
      <ref xlink:href="http://example.com/target"/>
      <sub_feat><Child gml:id="c1"/></sub_feat>
    </Item>
  </gml:featureMember>
  <gml:featureMember>
    <Item>
      <val class="B" flag="2" code="other" id="id2">20</val>
    </Item>
  </gml:featureMember>
  <gml:featureMember>
    <Item>
      <val class="A" flag="2" code="ok" id="id3">30</val>
    </Item>
  </gml:featureMember>
</root>
)xml";

constexpr std::string_view kConditionsGfs = R"xml(
<GMLFeatureClassList>
  <GMLFeatureClass>
    <Name>Item</Name>
    <ElementPath>Item</ElementPath>
    <PropertyDefn>
      <Name>val_a1</Name>
      <ElementPath>val</ElementPath>
      <Type>Integer</Type>
      <Condition>@class = 'A' and @flag = '1'</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>val_or</Name>
      <ElementPath>val</ElementPath>
      <Type>Integer</Type>
      <Condition>@class != 'A' or @flag = '2'</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>attr_code</Name>
      <ElementPath>val@code</ElementPath>
      <Type>String</Type>
      <Condition>@code = 'ok'</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>attr_id</Name>
      <ElementPath>val@id</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>ref_href</Name>
      <ElementPath>ref_href</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>sub_feat</Name>
      <ElementPath>sub_feat</ElementPath>
      <Type>FeatureProperty</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>bad_no_at</Name>
      <ElementPath>bad</ElementPath>
      <Type>String</Type>
      <Condition>no_at_sign</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>bad_no_eq</Name>
      <ElementPath>bad</ElementPath>
      <Type>String</Type>
      <Condition>@attr</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>bad_no_quote</Name>
      <ElementPath>bad</ElementPath>
      <Type>String</Type>
      <Condition>@attr = no_quote</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>bad_unclosed</Name>
      <ElementPath>bad</ElementPath>
      <Type>String</Type>
      <Condition>@attr = 'unclosed</Condition>
    </PropertyDefn>
    <PropertyDefn>
      <Name>bad_op</Name>
      <ElementPath>bad</ElementPath>
      <Type>String</Type>
      <Condition>@attr = 'v' bad_op @b = '1'</Condition>
    </PropertyDefn>
  </GMLFeatureClass>
</GMLFeatureClassList>
)xml";

TEST(GMLHandlerTest, GfsConditionEvaluationAndSyntaxErrors) {
  constexpr char kGmlPath[] = "/vsimem/gmlhandler_test/cond.gml";
  constexpr char kGfsPath[] = "/vsimem/gmlhandler_test/cond.gfs";
  VsiMemTempWrapper f_gml(kGmlPath, std::string(kConditionsGml));
  VsiMemTempWrapper f_gfs(kGfsPath, std::string(kConditionsGfs));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kGmlPath);
  EXPECT_TRUE(reader->LoadClasses(kGfsPath));
  reader->SetClassListLocked(true);

  WithQuietHandler quiet_handler;
  reader->ResetReading();
  int count = 0;
  while (std::unique_ptr<GMLFeature> feat{reader->NextFeature()}) {
    count++;
  }
  EXPECT_EQ(3, count);
}

constexpr std::string_view kAllGeometryTypesGml = R"xml(
<wfs:FeatureCollection xmlns:wfs="http://www.opengis.net/wfs"
                       xmlns:gml="http://www.opengis.net/gml"
                       numberOfFeatures="1"
                       numberMatched="1">
  <gml:boundedBy>
    <gml:Envelope srsName="EPSG:4326" srsDimension="3">
      <gml:lowerCorner>0 0 0</gml:lowerCorner>
      <gml:upperCorner>100 100 100</gml:upperCorner>
    </gml:Envelope>
  </gml:boundedBy>
  <gml:featureMember>
    <Feature>
      <gml:boundedBy srsDimension="3" srsName="EPSG:4326">
        <gml:Envelope srsDimension="3">
          <gml:lowerCorner>10 20 0</gml:lowerCorner>
          <gml:upperCorner>30 40 100</gml:upperCorner>
        </gml:Envelope>
      </gml:boundedBy>
      <geom_pt>
        <gml:Point srsName="EPSG:4326"><gml:coordinates>10,20</gml:coordinates></gml:Point>
      </geom_pt>
      <geom_ls>
        <gml:LineString><gml:posList>10 20 0 30 40 0</gml:posList></gml:LineString>
      </geom_ls>
      <geom_poly>
        <gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>0,0 10,0 10,10 0,10 0,0</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon>
      </geom_poly>
      <geom_mp>
        <gml:MultiPoint><gml:pointMember><gml:Point><gml:coordinates>1,1</gml:coordinates></gml:Point></gml:pointMember></gml:MultiPoint>
      </geom_mp>
      <geom_mls>
        <gml:MultiLineString><gml:lineStringMember><gml:LineString><gml:coordinates>1,1 2,2</gml:coordinates></gml:LineString></gml:lineStringMember></gml:MultiLineString>
      </geom_mls>
      <geom_mpoly>
        <gml:MultiPolygon><gml:polygonMember><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>0,0 1,0 1,1 0,1 0,0</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon></gml:polygonMember></gml:MultiPolygon>
      </geom_mpoly>
      <geom_tc>
        <gml:TopoCurve><gml:directedEdge><gml:Edge><gml:curveProperty><gml:LineString><gml:coordinates>0,0 1,1</gml:coordinates></gml:LineString></gml:curveProperty></gml:Edge></gml:directedEdge></gml:TopoCurve>
      </geom_tc>
      <geom_cs>
        <gml:CompositeSurface><gml:surfaceMember><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>0,0 2,0 2,2 0,2 0,0</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon></gml:surfaceMember></gml:CompositeSurface>
      </geom_cs>
      <geom_solid>
        <gml:Solid><gml:exterior><gml:CompositeSurface><gml:surfaceMember><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>0,0 1,0 1,1 0,1 0,0</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon></gml:surfaceMember></gml:CompositeSurface></gml:exterior></gml:Solid>
      </geom_solid>
      <time_prop>
        <timePosition>2026-11-03</timePosition>
      </time_prop>
    </Feature>
  </gml:featureMember>
</wfs:FeatureCollection>
)xml";

TEST(GMLHandlerTest, WfsCollectionAndAllGeometryTypes) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/all_geoms.gml";
  VsiMemTempWrapper file(kPath, std::string(kAllGeometryTypesGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  EXPECT_EQ(1, reader->GetClassCount());

  reader->ResetReading();
  std::unique_ptr<GMLFeature> feat(reader->NextFeature());
  ASSERT_NE(nullptr, feat);
}

constexpr std::string_view kCityGmlSample = R"xml(
<CityModel xmlns="http://www.opengis.net/citygml/2.0" xmlns:gml="http://www.opengis.net/gml">
  <cityObjectMember>
    <Building gml:id="b1">
      <boundedBy>
        <Envelope srsDimension="3">
          <lowerCorner>0 0 0</lowerCorner>
          <upperCorner>10 10 10</upperCorner>
        </Envelope>
      </boundedBy>
      <stringAttribute name="material">
        <value>brick</value>
      </stringAttribute>
      <intAttribute name="storeys">
        <value>3</value>
      </intAttribute>
      <doubleAttribute name="height">
        <value>12.5</value>
      </doubleAttribute>
      <lod2Solid>
        <gml:Solid>
          <gml:exterior>
            <gml:CompositeSurface>
              <gml:surfaceMember>
                <gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:posList>0 0 0 10 0 0 10 10 0 0 10 0 0 0 0</gml:posList></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon>
              </gml:surfaceMember>
            </gml:CompositeSurface>
          </gml:exterior>
        </gml:Solid>
      </lod2Solid>
    </Building>
  </cityObjectMember>
</CityModel>
)xml";

TEST(GMLHandlerTest, CityGMLGenericAttributesAndBoundedBy) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/citygml.gml";
  VsiMemTempWrapper file(kPath, std::string(kCityGmlSample));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  EXPECT_EQ(1, reader->GetClassCount());

  reader->ResetReading();
  std::unique_ptr<GMLFeature> feat(reader->NextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("b1", feat->GetFID());
}

constexpr std::string_view kBoundingBoxGeomGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <Feature>
      <boundedBy>
        <BoundingBox crs="EPSG:4326">
          <lowerCorner>10 20</lowerCorner>
          <upperCorner>30 40</upperCorner>
        </BoundingBox>
      </boundedBy>
      <prop>BBox</prop>
    </Feature>
  </gml:featureMember>
</root>
)xml";

TEST(GMLHandlerTest, FeatureBoundingBoxGeometry) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/bbox.gml";
  VsiMemTempWrapper file(kPath, std::string(kBoundingBoxGeomGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);
  reader->SetUseBBOX(true);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  reader->ResetReading();
  std::unique_ptr<GMLFeature> feat(reader->NextFeature());
  ASSERT_NE(nullptr, feat);
}

constexpr std::string_view kWfsJointLayerGml = R"xml(
<wfs:FeatureCollection xmlns:wfs="http://www.opengis.net/wfs" xmlns:gml="http://www.opengis.net/gml">
  <wfs:member>
    <wfs:Tuple>
      <wfs:member>
        <LayerA gml:id="a.1">
          <geom><gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point></geom>
        </LayerA>
      </wfs:member>
      <wfs:member>
        <LayerB gml:id="b.1">
          <geom><gml:Point><gml:coordinates>3,4</gml:coordinates></gml:Point></geom>
        </LayerB>
      </wfs:member>
    </wfs:Tuple>
  </wfs:member>
</wfs:FeatureCollection>
)xml";

TEST(GMLHandlerTest, WfsJointLayerReading) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/wfs_joint.gml";
  VsiMemTempWrapper file(kPath, std::string(kWfsJointLayerGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);
  reader->SetIsWFSJointLayer(true);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  reader->ResetReading();
  std::unique_ptr<GMLFeature> feat(reader->NextFeature());
  ASSERT_NE(nullptr, feat);
}

constexpr std::string_view kInspireGeometryGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <InspireFeature>
      <geometry>
        <gml:Point><gml:coordinates>10,20</gml:coordinates></gml:Point>
      </geometry>
      <otherGeometry>
        <gml:Point><gml:coordinates>30,40</gml:coordinates></gml:Point>
      </otherGeometry>
      <name>Test</name>
    </InspireFeature>
  </gml:featureMember>
</root>
)xml";

TEST(GMLHandlerTest, InspireGeometryAndFetchAllGeometries) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/inspire.gml";
  VsiMemTempWrapper file(kPath, std::string(kInspireGeometryGml));

  // 1. Default: Inspire picks <geometry> first, ignores subsequent.
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kPath);
    EXPECT_TRUE(reader->PrescanForSchema(true, false));
    reader->ResetReading();
    std::unique_ptr<GMLFeature> feat(reader->NextFeature());
    ASSERT_NE(nullptr, feat);
  }

  // 2. FetchAllGeometries: collects all geometries.
  {
    CPLSetConfigOption("GML_FETCH_ALL_GEOMETRIES", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GML_FETCH_ALL_GEOMETRIES", nullptr);
    };

    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kPath);
    EXPECT_TRUE(reader->PrescanForSchema(true, false));
    reader->ResetReading();
    std::unique_ptr<GMLFeature> feat(reader->NextFeature());
    ASSERT_NE(nullptr, feat);
  }
}

constexpr std::string_view kMultiClassGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <ClassA><name>A1</name></ClassA>
  </gml:featureMember>
  <gml:featureMember>
    <ClassB><name>B1</name></ClassB>
  </gml:featureMember>
</root>
)xml";

constexpr std::string_view kClassBGfs = R"xml(
<GMLFeatureClassList>
  <GMLFeatureClass>
    <Name>ClassB</Name>
    <ElementPath>ClassB</ElementPath>
    <PropertyDefn>
      <Name>name</Name>
      <ElementPath>name</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
  </GMLFeatureClass>
</GMLFeatureClassList>
)xml";

TEST(GMLHandlerTest, FilteredClassNameAndStateIgnoredFeature) {
  constexpr char kGmlPath[] = "/vsimem/gmlhandler_test/filtered.gml";
  constexpr char kGfsPath[] = "/vsimem/gmlhandler_test/filtered.gfs";
  VsiMemTempWrapper f_gml(kGmlPath, std::string(kMultiClassGml));
  VsiMemTempWrapper f_gfs(kGfsPath, std::string(kClassBGfs));

  // 1. Filtered class without locked list.
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kGmlPath);
    EXPECT_TRUE(reader->PrescanForSchema(true, false));
    reader->ResetReading();
    reader->SetFilteredClassName("ClassB");
    std::unique_ptr<GMLFeature> feat(reader->NextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_STREQ("ClassB", feat->GetClass()->GetName());
    EXPECT_EQ(nullptr, reader->NextFeature());
  }

  // 2. Filtered class with locked class list (ShouldLookForClassAtAnyLevel =
  // true).
  {
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kGmlPath);
    EXPECT_TRUE(reader->LoadClasses(kGfsPath));
    reader->SetClassListLocked(true);
    reader->ResetReading();
    reader->SetFilteredClassName("ClassB");
    std::unique_ptr<GMLFeature> feat(reader->NextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_STREQ("ClassB", feat->GetClass()->GetName());
    EXPECT_EQ(nullptr, reader->NextFeature());
  }
}

TEST(GMLHandlerTest, MaxDepthLimitAndUnlimitedOption) {
  // Generate XML with 66 levels of nested elements.
  std::string open_tags;
  std::string close_tags;
  for (int i = 0; i < 66; ++i) {
    open_tags += std::format("<d{}>", i);
    close_tags = std::format("</d{}>", i) + close_tags;
  }
  std::string deep_xml = open_tags + close_tags;

  constexpr char kPath[] = "/vsimem/gmlhandler_test/deep.xml";
  VsiMemTempWrapper file(kPath, deep_xml);

  // 1. Default depth limit triggers failure at depth 64.
  {
    WithQuietHandler quiet_handler;
    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kPath);
    CPLErrorReset();
    reader->PrescanForSchema(true, false);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. UNLIMITED option allows parsing.
  {
    CPLSetConfigOption("OGR_GML_NESTING_LEVEL", "UNLIMITED");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("OGR_GML_NESTING_LEVEL", nullptr);
    };

    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kPath);
    // Deep XML has no feature definitions, so prescan succeeds with 0 classes.
    EXPECT_TRUE(reader->PrescanForSchema(true, false));
  }
}

constexpr std::string_view kAttributeVariantsGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xlink="http://www.w3.org/1999/xlink">
  <gml:featureMember>
    <Feature>
      <nil_prop xsi:nil="true"/>
      <link_prop xlink:href="http://example.com/target"/>
      <attr_prop code="123" uom="m">456</attr_prop>
      <empty_prop></empty_prop>
      <empty_attr code=""/>
    </Feature>
  </gml:featureMember>
</root>
)xml";

TEST(GMLHandlerTest, AttributeVariantsAndReportAllAttributes) {
  constexpr char kPath[] = "/vsimem/gmlhandler_test/attrs.gml";
  VsiMemTempWrapper file(kPath, std::string(kAttributeVariantsGml));

  // 1. With EmptyAsNull=false and ReportAllAttributes=true.
  {
    CPLSetConfigOption("GML_ATTRIBUTES_TO_OGR_FIELDS", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GML_ATTRIBUTES_TO_OGR_FIELDS", nullptr);
    };

    auto reader =
        std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
    ASSERT_NE(nullptr, reader);
    reader->SetSourceFile(kPath);
    reader->SetEmptyAsNull(false);
    reader->SetReportAllAttributes(true);

    EXPECT_TRUE(reader->PrescanForSchema(true, false));
    EXPECT_EQ(1, reader->GetClassCount());

    reader->ResetReading();
    std::unique_ptr<GMLFeature> feat(reader->NextFeature());
    ASSERT_NE(nullptr, feat);
  }
}

TEST(GMLHandlerTest, LargeAttributeBufferReallocation) {
  std::string large_val(3000, 'x');
  std::string gml =
      "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember><F><"
      "desc>";
  gml += large_val;
  gml += "</desc></F></gml:featureMember></root>";

  constexpr char kPath[] = "/vsimem/gmlhandler_test/large_attr.gml";
  VsiMemTempWrapper file(kPath, gml);

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  EXPECT_TRUE(reader->PrescanForSchema(true, false));
  reader->ResetReading();
  std::unique_ptr<GMLFeature> feat(reader->NextFeature());
  ASSERT_NE(nullptr, feat);
  ASSERT_NE(nullptr, feat->GetProperty(0));
  ASSERT_GT(feat->GetProperty(0)->nSubProperties, 0);
  EXPECT_EQ(3000, strlen(feat->GetProperty(0)->papszSubProperties[0]));
}

TEST(GMLHandlerTest, MillionLaughsPatternDetection) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  XML_Parser oParser = OGRCreateExpatXMLParser();
  ASSERT_NE(nullptr, oParser);
  absl::Cleanup parser_closer = [oParser] { XML_ParserFree(oParser); };

  GMLExpatHandler handler(reader.get(), oParser);
  EXPECT_FALSE(handler.HasStoppedParsing());

  // Simulate million laughs expansion exceeding PARSER_BUF_SIZE (10 * 8192).
  {
    WithQuietHandler quiet_handler;
    for (int i = 0; i < 81920 + 10; ++i) {
      GMLExpatHandler::dataHandlerCbk(&handler, "a", 1);
    }
  }
  EXPECT_TRUE(handler.HasStoppedParsing());
}

TEST(GMLHandlerTest, GMLExpatHandlerDirectMethods) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  XML_Parser oParser = OGRCreateExpatXMLParser();
  ASSERT_NE(nullptr, oParser);
  absl::Cleanup parser_closer = [oParser] { XML_ParserFree(oParser); };

  GMLExpatHandler handler(reader.get(), oParser);
  EXPECT_FALSE(handler.HasStoppedParsing());
  handler.ResetDataHandlerCounter();

  const char* attrs[] = {"fid", "fid.99", "code", "xyz", nullptr};
  EXPECT_STREQ("fid.99", handler.GetFID(reinterpret_cast<void*>(attrs)));

  std::unique_ptr<char, CplFreeDeleter> val(
      handler.GetAttributeValue(reinterpret_cast<void*>(attrs), "code"));
  ASSERT_NE(nullptr, val);
  EXPECT_STREQ("xyz", val.get());

  EXPECT_EQ(nullptr, handler.GetAttributeValue(reinterpret_cast<void*>(attrs),
                                               "missing"));

  char* raw_key = nullptr;
  std::unique_ptr<char, CplFreeDeleter> val0(
      handler.GetAttributeByIdx(reinterpret_cast<void*>(attrs), 0, &raw_key));
  std::unique_ptr<char, CplFreeDeleter> key(raw_key);
  ASSERT_NE(nullptr, val0);
  ASSERT_NE(nullptr, key);
  EXPECT_STREQ("fid", key.get());
  EXPECT_STREQ("fid.99", val0.get());

  EXPECT_EQ(nullptr, handler.GetAttributeByIdx(reinterpret_cast<void*>(attrs),
                                               2, &raw_key));

  CPLXMLTreeCloser node(CPLCreateXMLNode(nullptr, CXT_Element, "test"));
  handler.AddAttributes(node.get(), reinterpret_cast<void*>(attrs));
  EXPECT_NE(nullptr, node->psChild);
}

}  // namespace
}  // namespace autotest2
