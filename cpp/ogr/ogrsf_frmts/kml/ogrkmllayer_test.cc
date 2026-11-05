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
// Tests OGRKMLLayer (ogrkmllayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/kml/ogrkmllayer.cpp

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/ogr_kml.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(OgrKmlLayerTest, SrsAndTransformationBranches) {
  constexpr char kOut[] = "/vsimem/ogrkmllayer_test/srs_test.kml";
  absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

  OGRKMLDataSource ds;
  ASSERT_TRUE(ds.Create(kOut, nullptr));

  // 1. null SRS
  OGRLayer* layerNull = ds.ICreateLayer("LayerNull", nullptr, nullptr);
  ASSERT_NE(nullptr, layerNull);
  EXPECT_EQ(&ds, layerNull->GetDataset());

  // 2. WGS84 SRS (IsSame is true, poCT_ is null)
  OGRSpatialReference srsWgs84;
  srsWgs84.SetWellKnownGeogCS("WGS84");
  srsWgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  OGRGeomFieldDefn geomDefnWgs84("geom", wkbPoint);
  geomDefnWgs84.SetSpatialRef(&srsWgs84);
  OGRLayer* layerWgs84 = ds.ICreateLayer("LayerWgs84", &geomDefnWgs84, nullptr);
  ASSERT_NE(nullptr, layerWgs84);

  // 3. EPSG:3857 SRS (creates poCT_)
  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  OGRGeomFieldDefn geomDefn3857("geom", wkbPoint);
  geomDefn3857.SetSpatialRef(&srs3857);
  OGRLayer* layer3857 = ds.ICreateLayer("Layer3857", &geomDefn3857, nullptr);
  ASSERT_NE(nullptr, layer3857);

  // 4. Untransformable custom SRS -> emits warning on first, sets
  // bIssuedCTError
  OGRSpatialReference srsCustom;
  srsCustom.SetLocalCS("UntransformableLocalCS");
  OGRGeomFieldDefn geomDefnCustom("geom", wkbPoint);
  geomDefnCustom.SetSpatialRef(&srsCustom);

  EXPECT_TRUE(ds.IsFirstCTError());
  {
    WithQuietHandler quiet_handler;
    OGRLayer* layerCust1 =
        ds.ICreateLayer("LayerCustom1", &geomDefnCustom, nullptr);
    CPLErrorReset();
    ASSERT_NE(nullptr, layerCust1);
  }
  EXPECT_FALSE(ds.IsFirstCTError());

  // Second untransformable SRS layer -> does NOT emit warning again
  {
    OGRLayer* layerCust2 =
        ds.ICreateLayer("LayerCustom2", &geomDefnCustom, nullptr);
    ASSERT_NE(nullptr, layerCust2);
  }
}

TEST(OgrKmlLayerTest, CapabilitiesAndFieldCreation) {
  constexpr char kOut[] = "/vsimem/ogrkmllayer_test/cap_test.kml";
  absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

  OGRKMLDataSource ds;
  ASSERT_TRUE(ds.Create(kOut, nullptr));

  OGRLayer* layer = ds.ICreateLayer("CapLayer", nullptr, nullptr);
  ASSERT_NE(nullptr, layer);

  // TestCapability for writer layer before any features
  EXPECT_TRUE(layer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(layer->TestCapability(OLCCreateField));
  EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCZGeometries));
  EXPECT_FALSE(layer->TestCapability("NonExistentCap"));

  // CreateField succeeds
  OGRFieldDefn fld1("Field1", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fld1));

  // Write a feature
  OGRFeature feat(layer->GetLayerDefn());
  feat.SetField("Field1", 123);
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));

  // Now OLCCreateField should return FALSE and CreateField should fail
  EXPECT_FALSE(layer->TestCapability(OLCCreateField));
  OGRFieldDefn fld2("Field2", OFTString);
  EXPECT_EQ(OGRERR_FAILURE, layer->CreateField(&fld2));
}

TEST(OgrKmlLayerTest, WriteSchemaAllDataTypes) {
  constexpr char kOut[] = "/vsimem/ogrkmllayer_test/schema_test.kml";
  absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

  OGRKMLDataSource ds;
  ASSERT_TRUE(ds.Create(kOut, nullptr));

  OGRKMLLayer* layer = static_cast<OGRKMLLayer*>(
      ds.ICreateLayer("SchemaLayer", nullptr, nullptr));
  ASSERT_NE(nullptr, layer);

  // Add all data types
  OGRFieldDefn fInt("fInt", OFTInteger);
  OGRFieldDefn fIntList("fIntList", OFTIntegerList);
  OGRFieldDefn fReal("fReal", OFTReal);
  OGRFieldDefn fRealList("fRealList", OFTRealList);
  OGRFieldDefn fStr("fStr", OFTString);
  OGRFieldDefn fStrList("fStrList", OFTStringList);
  OGRFieldDefn fDate("fDate", OFTDate);
  OGRFieldDefn fTime("fTime", OFTTime);
  OGRFieldDefn fDateTime("fDateTime", OFTDateTime);
  OGRFieldDefn fBinary("fBinary", OFTBinary);

  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fInt));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fIntList));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fReal));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fRealList));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fStr));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fStrList));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fDate));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fTime));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fDateTime));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fBinary));

  CPLString schema = layer->WriteSchema();
  EXPECT_THAT(schema,
              HasSubstr("<Schema name=\"SchemaLayer\" id=\"SchemaLayer\">"));
  EXPECT_THAT(schema, HasSubstr("<SimpleField name=\"fInt\" type=\"int\">"));
  EXPECT_THAT(schema,
              HasSubstr("<SimpleArrayField name=\"fIntList\" type=\"int\">"));
  EXPECT_THAT(schema, HasSubstr("<SimpleField name=\"fReal\" type=\"float\">"));
  EXPECT_THAT(
      schema,
      HasSubstr("<SimpleArrayField name=\"fRealList\" type=\"float\">"));
  EXPECT_THAT(schema, HasSubstr("<SimpleField name=\"fStr\" type=\"string\">"));
  EXPECT_THAT(
      schema,
      HasSubstr("<SimpleArrayField name=\"fStrList\" type=\"string\">"));
  EXPECT_THAT(schema,
              HasSubstr("<SimpleField name=\"fDate\" type=\"string\">"));
  EXPECT_THAT(schema,
              HasSubstr("<SimpleField name=\"fTime\" type=\"string\">"));
  EXPECT_THAT(schema,
              HasSubstr("<SimpleField name=\"fDateTime\" type=\"string\">"));
  EXPECT_THAT(schema,
              HasSubstr("<SimpleField name=\"fBinary\" type=\"string\">"));
  EXPECT_THAT(schema, HasSubstr("</Schema>"));
}

TEST(OgrKmlLayerTest, ICreateFeatureBranches) {
  constexpr char kOut[] = "/vsimem/ogrkmllayer_test/feat_branches.kml";
  absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

  {
    OGRKMLDataSource ds;
    ASSERT_TRUE(ds.Create(kOut, nullptr));

    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRGeomFieldDefn geomDefn("geom", wkbPolygon);
    geomDefn.SetSpatialRef(&srs3857);

    OGRKMLLayer* layer1 = static_cast<OGRKMLLayer*>(
        ds.ICreateLayer("Layer1", &geomDefn, nullptr));
    ASSERT_NE(nullptr, layer1);

    OGRFieldDefn fldReal("DoubleVal", OFTReal);
    EXPECT_EQ(OGRERR_NONE, layer1->CreateField(&fldReal));

    // Feature with:
    // - Leading spaces in Name & Description
    // - Style string with pen (8-char hex color #11223344 and width) + non-pen
    // tool
    // - Polygon geometry in EPSG:3857 (reprojected via poCT_)
    // - OFTReal field
    OGRFeature feat1(layer1->GetLayerDefn());
    feat1.SetField("Name", "   LeadingSpacesName");
    feat1.SetField("Description", "   LeadingSpacesDesc");
    feat1.SetField("DoubleVal", 42.5);
    feat1.SetStyleString("BRUSH(fc:#00ff00);PEN(c:#11223344,w:5px)");

    OGRPolygon poly1;
    OGRLinearRing ring1;
    // EPSG:3857 coordinates near origin (0,0)
    ring1.addPoint(0, 0);
    ring1.addPoint(1000, 0);
    ring1.addPoint(1000, 1000);
    ring1.addPoint(0, 1000);
    ring1.addPoint(0, 0);
    poly1.addRing(&ring1);
    feat1.SetGeometry(&poly1);

    EXPECT_EQ(OGRERR_NONE, layer1->ICreateFeature(&feat1));

    // Feature with 6-char hex color and default width
    OGRFeature feat2(layer1->GetLayerDefn());
    feat2.SetField("Name", "SixHexColor");
    feat2.SetStyleString("PEN(c:#112233)");
    OGRLineString ls1;
    ls1.addPoint(0, 0);
    ls1.addPoint(1000, 1000);
    feat2.SetGeometry(&ls1);
    EXPECT_EQ(OGRERR_NONE, layer1->ICreateFeature(&feat2));

    // Feature with short/invalid hex color -> defaults to ff0000ff
    OGRFeature feat3(layer1->GetLayerDefn());
    feat3.SetStyleString("PEN(c:#1234)");
    OGRLineString ls2;
    ls2.addPoint(10, 10);
    ls2.addPoint(20, 20);
    feat3.SetGeometry(&ls2);
    EXPECT_EQ(OGRERR_NONE, layer1->ICreateFeature(&feat3));

    // Feature with unsupported geometry for KML export -> error branch
    OGRFeature feat4(layer1->GetLayerDefn());
    OGRTriangle tri;
    feat4.SetGeometry(&tri);
    {
      WithQuietHandler quiet_handler;
      layer1->ICreateFeature(&feat4);
      CPLErrorReset();
    }

    // Create second layer -> closes layer1 for writing
    OGRLayer* layer2 = ds.ICreateLayer("Layer2", nullptr, nullptr);
    ASSERT_NE(nullptr, layer2);

    // Calling ICreateFeature on layer1 now fails because bClosedForWriting is
    // true
    {
      WithQuietHandler quiet_handler;
      EXPECT_EQ(OGRERR_FAILURE, layer1->ICreateFeature(&feat1));
      CPLErrorReset();
    }
  }

  // Verify flushed output file contents
  vsi_l_offset nSize = 0;
  GByte* pabyData = VSIGetMemFileBuffer(kOut, &nSize, FALSE);
  ASSERT_NE(nullptr, pabyData);
  std::string content(reinterpret_cast<char*>(pabyData), nSize);
  // Verify color conversion (#11223344 -> 44332211) and width
  EXPECT_THAT(content, HasSubstr("<color>44332211</color>"));
  EXPECT_THAT(content, HasSubstr("<width>5</width>"));
  // Verify 6-digit hex (#112233 -> FF332211)
  EXPECT_THAT(content, HasSubstr("<color>FF332211</color>"));
  // Verify fallback color
  EXPECT_THAT(content, HasSubstr("<color>ff0000ff</color>"));
  // Verify Polygon tag
  EXPECT_THAT(content, HasSubstr("<Polygon>"));
}

TEST(OgrKmlLayerTest, ReadFeaturesAndFiltering) {
  constexpr char kKml[] = R"xml(<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Folder><name>LayerA</name>
      <Placemark><name>FeatureOne</name><description>DescOne</description>
        <Point><coordinates>1,1,10</coordinates></Point>
      </Placemark>
      <Placemark><name>FeatureTwo</name><description>DescTwo</description>
        <Point><coordinates>10,10,20</coordinates></Point>
      </Placemark>
      <Placemark><name>FeatureThree</name><description>DescThree</description>
        <Point><coordinates>20,20,30</coordinates></Point>
      </Placemark>
    </Folder>
  </Document>
</kml>)xml";

  constexpr char kPath[] = "/vsimem/ogrkmllayer_test/read_test.kml";
  VsiMemTempWrapper f(kPath, kKml);

  OGRKMLDataSource ds;
  ASSERT_TRUE(ds.Open(kPath, TRUE));
  ASSERT_EQ(1, ds.GetLayerCount());

  OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
  ASSERT_NE(nullptr, layer);

  // TestCapability on reader layer
  EXPECT_FALSE(layer->TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(layer->TestCapability(OLCCreateField));
  EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCZGeometries));

  // ICreateFeature and CreateField fail on reader
  OGRFieldDefn dummyField("dummy", OFTInteger);
  EXPECT_EQ(OGRERR_FAILURE, layer->CreateField(&dummyField));
  OGRFeature dummyFeat(layer->GetLayerDefn());
  EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&dummyFeat));

  // Unfiltered feature count
  EXPECT_EQ(3, layer->GetFeatureCount());

  // Iterate features using std::unique_ptr for RAII
  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("FeatureOne", feat->GetFieldAsString("Name"));
  EXPECT_STREQ("DescOne", feat->GetFieldAsString("Description"));
  EXPECT_EQ(0, feat->GetFID());

  feat.reset(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("FeatureTwo", feat->GetFieldAsString("Name"));
  EXPECT_EQ(1, feat->GetFID());

  feat.reset(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("FeatureThree", feat->GetFieldAsString("Name"));
  EXPECT_EQ(2, feat->GetFID());

  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // ResetReading
  layer->ResetReading();
  feat.reset(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("FeatureOne", feat->GetFieldAsString("Name"));

  // Spatial filter: box enclosing (0,0) to (5,5) -> matches only FeatureOne
  // (1,1)
  layer->SetSpatialFilterRect(0, 0, 5, 5);
  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  feat.reset(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("FeatureOne", feat->GetFieldAsString("Name"));
  EXPECT_EQ(nullptr, layer->GetNextFeature());
  layer->SetSpatialFilter(nullptr);

  // Attribute filter: Name = 'FeatureTwo' -> matches only FeatureTwo
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("Name = 'FeatureTwo'"));
  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  feat.reset(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("FeatureTwo", feat->GetFieldAsString("Name"));
  EXPECT_EQ(nullptr, layer->GetNextFeature());
  layer->SetAttributeFilter(nullptr);
}

}  // namespace
}  // namespace autotest2
