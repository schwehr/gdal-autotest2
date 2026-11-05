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
// Tests OGRKMLDataSource (ogrkmldatasource.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/kml/ogrkmldatasource.cpp

#include <filesystem>  // NOLINT
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/ogr_kml.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(OgrKmlDataSourceTest, OpenFailureBranches) {
  // 1. Non-existent file
  {
    OGRKMLDataSource ds;
    EXPECT_FALSE(ds.Open("/vsimem/nonexistent_file.kml", TRUE));
    EXPECT_FALSE(ds.Open("/vsimem/nonexistent_file.kml", FALSE));
  }

  // 2. Non-KML file with bTestOpen = TRUE
  {
    constexpr char kNotKml[] = "<not_kml><Document></Document></not_kml>";
    VsiMemTempWrapper f("/vsimem/ogrkmldatasource_test/not_kml.xml", kNotKml);
    OGRKMLDataSource ds;
    EXPECT_FALSE(ds.Open("/vsimem/ogrkmldatasource_test/not_kml.xml", TRUE));
  }

  // 3. Valid XML header but broken parse
  {
    constexpr char kBadXml[] =
        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><unclosed>";
    VsiMemTempWrapper f("/vsimem/ogrkmldatasource_test/bad_xml.kml", kBadXml);
    OGRKMLDataSource ds;
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(ds.Open("/vsimem/ogrkmldatasource_test/bad_xml.kml", TRUE));
    CPLErrorReset();
  }

  // 4. File that fails classifyNodes (exceeds recursion limit of 32 in
  // kmlnode.cpp)
  {
    std::string deep = "<kml xmlns=\"http://www.opengis.net/kml/2.2\">";
    for (int i = 0; i < 35; ++i) {
      deep += "<Folder>";
    }
    deep +=
        "<Placemark><Point><coordinates>0,0</coordinates></Point></Placemark>";
    for (int i = 0; i < 35; ++i) {
      deep += "</Folder>";
    }
    deep += "</kml>";
    VsiMemTempWrapper f("/vsimem/ogrkmldatasource_test/deep.kml", deep);
    OGRKMLDataSource ds;
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(ds.Open("/vsimem/ogrkmldatasource_test/deep.kml", TRUE));
    CPLErrorReset();
  }
}

TEST(OgrKmlDataSourceTest, OpenWithEmptyContainersAndDebug) {
  constexpr char kEmptyKml[] =
      "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>"
      "<Folder><name>EmptyFolder</name></Folder>"
      "</Document></kml>";
  VsiMemTempWrapper f("/vsimem/ogrkmldatasource_test/empty.kml", kEmptyKml);

  OGRKMLDataSource ds;
  {
    CPLConfigOptionSetter debug_setter("KML_DEBUG", "ON", false);
    EXPECT_TRUE(ds.Open("/vsimem/ogrkmldatasource_test/empty.kml", TRUE));
  }

  EXPECT_EQ(1, ds.GetLayerCount());
  EXPECT_EQ(nullptr, ds.GetLayer(-1));
  EXPECT_NE(nullptr, ds.GetLayer(0));
  EXPECT_STREQ("EmptyFolder", ds.GetLayer(0)->GetName());
  EXPECT_EQ(nullptr, ds.GetLayer(1));
}

TEST(OgrKmlDataSourceTest, OpenVariousGeometryTypesAndLayerNames) {
  constexpr char kMultiLayerKml[] =
      R"xml(<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <!-- 2.5D Point layer -->
    <Folder><name>PointLayer</name>
      <Placemark><Point><coordinates>1,2,3</coordinates></Point></Placemark>
    </Folder>
    <!-- LineString layer -->
    <Folder><name>LineLayer</name>
      <Placemark><LineString><coordinates>1,2 3,4</coordinates></LineString></Placemark>
    </Folder>
    <!-- Polygon layer -->
    <Folder><name>PolyLayer</name>
      <Placemark><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 1,0 1,1 0,1 0,0</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark>
    </Folder>
    <!-- MultiPoint layer -->
    <Folder><name>MultiPtLayer</name>
      <Placemark><MultiGeometry><Point><coordinates>1,1</coordinates></Point><Point><coordinates>2,2</coordinates></Point></MultiGeometry></Placemark>
    </Folder>
    <!-- MultiLineString layer -->
    <Folder><name>MultiLineLayer</name>
      <Placemark><MultiGeometry><LineString><coordinates>1,1 2,2</coordinates></LineString><LineString><coordinates>3,3 4,4</coordinates></LineString></MultiGeometry></Placemark>
    </Folder>
    <!-- MultiPolygon layer -->
    <Folder><name>MultiPolyLayer</name>
      <Placemark><MultiGeometry><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 1,0 1,1 0,0</coordinates></LinearRing></outerBoundaryIs></Polygon><Polygon><outerBoundaryIs><LinearRing><coordinates>2,2 3,2 3,3 2,2</coordinates></LinearRing></outerBoundaryIs></Polygon></MultiGeometry></Placemark>
    </Folder>
    <!-- Generic MultiGeometry layer (heterogeneous) -->
    <Folder><name>MixedGeomLayer</name>
      <Placemark><MultiGeometry><Point><coordinates>1,1</coordinates></Point><LineString><coordinates>1,1 2,2</coordinates></LineString></MultiGeometry></Placemark>
    </Folder>
    <!-- Unnamed layer -->
    <Folder>
      <Placemark><Point><coordinates>5,6</coordinates></Point></Placemark>
    </Folder>
    <!-- Duplicate layer name -->
    <Folder><name>PointLayer</name>
      <Placemark><Point><coordinates>7,8</coordinates></Point></Placemark>
    </Folder>
    <!-- Triplicate layer name -->
    <Folder><name>PointLayer</name>
      <Placemark><Point><coordinates>9,10</coordinates></Point></Placemark>
    </Folder>
  </Document>
</kml>)xml";

  VsiMemTempWrapper f("/vsimem/ogrkmldatasource_test/layers.kml",
                      kMultiLayerKml);
  OGRKMLDataSource ds;
  EXPECT_TRUE(ds.Open("/vsimem/ogrkmldatasource_test/layers.kml", TRUE));
  EXPECT_EQ(10, ds.GetLayerCount());

  struct ExpectedLayer {
    int index;
    const char* name;
    OGRwkbGeometryType geom_type;
  };

  constexpr ExpectedLayer kExpectedLayers[] = {
      {0, "PointLayer", wkbPoint25D},
      {1, "LineLayer", wkbLineString},
      {2, "PolyLayer", wkbPolygon},
      {3, "MultiPtLayer", wkbMultiPoint},
      {4, "MultiLineLayer", wkbMultiLineString},
      {5, "MultiPolyLayer", wkbMultiPolygon},
      {6, "MixedGeomLayer", wkbGeometryCollection},
      {7, "Layer #7", wkbPoint},
      {8, "PointLayer (#2)", wkbPoint},
      {9, "PointLayer (#3)", wkbPoint},
  };

  for (const auto& expected : kExpectedLayers) {
    const OGRLayer* layer = ds.GetLayer(expected.index);
    ASSERT_NE(nullptr, layer) << "Missing layer at index " << expected.index;
    EXPECT_STREQ(expected.name, layer->GetName());
    EXPECT_EQ(expected.geom_type, layer->GetGeomType());
  }

  EXPECT_EQ(nullptr, ds.GetLayer(10));
}

TEST(OgrKmlDataSourceTest, CreateAndOptions) {
  // 1. Create with invalid path
  {
    const std::string non_existent =
        (std::filesystem::path(::testing::TempDir()) / "does_not_exist" /
         "out.kml")
            .string();
    OGRKMLDataSource ds;
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(ds.Create(non_existent.c_str(), nullptr));
    CPLErrorReset();
  }

  // 2. Create with custom options including valid AltitudeMode
  {
    constexpr char kOut[] = "/vsimem/ogrkmldatasource_test/create_opt.kml";
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

    OGRKMLDataSource ds;
    CPLStringList options;
    options.SetNameValue("NameField", "CustomName");
    options.SetNameValue("DescriptionField", "CustomDesc");
    options.SetNameValue("DOCUMENT_ID", "doc_42");
    options.SetNameValue("AltitudeMode", "relativeToGround");

    EXPECT_TRUE(ds.Create(kOut, options.List()));

    EXPECT_STREQ("CustomName", ds.GetNameField());
    EXPECT_STREQ("CustomDesc", ds.GetDescriptionField());
    EXPECT_STREQ("relativeToGround", ds.GetAltitudeMode());
    EXPECT_NE(nullptr, ds.GetOutputFP());

    // Test capabilities
    EXPECT_TRUE(ds.TestCapability(ODsCCreateLayer));
    EXPECT_TRUE(ds.TestCapability(ODsCZGeometries));
    EXPECT_FALSE(ds.TestCapability("UnknownCapability"));

    // GrowExtents & CT Error flags
    OGREnvelope env;
    env.MinX = 0.0;
    env.MaxX = 10.0;
    env.MinY = 0.0;
    env.MaxY = 10.0;
    ds.GrowExtents(&env);

    EXPECT_TRUE(ds.IsFirstCTError());
    ds.IssuedFirstCTError();
    EXPECT_FALSE(ds.IsFirstCTError());
  }

  // 3. Create with invalid AltitudeMode (warning emitted, reset to nullptr)
  {
    constexpr char kOut[] = "/vsimem/ogrkmldatasource_test/bad_alt.kml";
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

    OGRKMLDataSource ds;
    CPLStringList options;
    options.SetNameValue("AltitudeMode", "invalidMode");
    WithQuietHandler quiet_handler;
    EXPECT_TRUE(ds.Create(kOut, options.List()));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.GetAltitudeMode());
  }

  // 4. Create with clampToGround and absolute AltitudeMode
  {
    constexpr char kOut1[] = "/vsimem/ogrkmldatasource_test/alt_clamp.kml";
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut1); };

    OGRKMLDataSource ds1;
    CPLStringList opt1;
    opt1.SetNameValue("AltitudeMode", "clampToGround");
    EXPECT_TRUE(ds1.Create(kOut1, opt1.List()));
    EXPECT_STREQ("clampToGround", ds1.GetAltitudeMode());
  }

  {
    constexpr char kOut2[] = "/vsimem/ogrkmldatasource_test/alt_abs.kml";
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut2); };

    OGRKMLDataSource ds2;
    CPLStringList opt2;
    opt2.SetNameValue("AltitudeMode", "absolute");
    EXPECT_TRUE(ds2.Create(kOut2, opt2.List()));
    EXPECT_STREQ("absolute", ds2.GetAltitudeMode());
  }
}

TEST(OgrKmlDataSourceTest, ICreateLayerAndDestructorFlushing) {
  // 1. ICreateLayer on read-only datasource -> returns nullptr
  {
    OGRKMLDataSource ds;
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, ds.ICreateLayer("Layer1", nullptr, nullptr));
    CPLErrorReset();
  }

  // 2. Destructor with 1 layer and 0 features written
  constexpr char kOut1[] = "/vsimem/ogrkmldatasource_test/destruct_0feat.kml";
  {
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut1); };
    {
      OGRKMLDataSource ds;
      EXPECT_TRUE(ds.Create(kOut1, nullptr));
      OGRLayer* layer = ds.ICreateLayer("TestLayer", nullptr, nullptr);
      ASSERT_NE(nullptr, layer);
    }
    // Check output content
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(kOut1, &nSize, FALSE);
    ASSERT_NE(nullptr, pabyData);
    std::string content(reinterpret_cast<char*>(pabyData), nSize);
    EXPECT_THAT(content, HasSubstr("<Folder><name>TestLayer</name>"));
    EXPECT_THAT(content, HasSubstr("</Folder>"));
    EXPECT_THAT(content, HasSubstr("</Document></kml>"));
  }

  // 3. ICreateLayer with XML-unsafe name and multiple layers with features
  constexpr char kOut2[] =
      "/vsimem/ogrkmldatasource_test/destruct_multilyr.kml";
  {
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut2); };
    {
      OGRKMLDataSource ds;
      EXPECT_TRUE(ds.Create(kOut2, nullptr));

      // First layer: name with characters needing cleaning
      WithQuietHandler quiet_handler;
      OGRLayer* layer1 = ds.ICreateLayer("Layer & 1 <bad>", nullptr, nullptr);
      CPLErrorReset();
      ASSERT_NE(nullptr, layer1);

      OGRFieldDefn fldInt("IntegerField", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, layer1->CreateField(&fldInt));

      // Write a feature to layer1
      OGRFeature feat1(layer1->GetLayerDefn());
      feat1.SetField("IntegerField", 100);
      OGRPoint pt1(1.0, 2.0);
      feat1.SetGeometry(&pt1);
      EXPECT_EQ(OGRERR_NONE, layer1->CreateFeature(&feat1));

      // Second layer: creating it closes layer1 and marks layer1 closed for
      // writing
      OGRLayer* layer2 = ds.ICreateLayer("Layer2", nullptr, nullptr);
      ASSERT_NE(nullptr, layer2);

      OGRFieldDefn fldStr("StrField", OFTString);
      EXPECT_EQ(OGRERR_NONE, layer2->CreateField(&fldStr));

      // Write a feature to layer2
      OGRFeature feat2(layer2->GetLayerDefn());
      feat2.SetField("StrField", "Hello");
      OGRPoint pt2(3.0, 4.0);
      feat2.SetGeometry(&pt2);
      EXPECT_EQ(OGRERR_NONE, layer2->CreateFeature(&feat2));
    }
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(kOut2, &nSize, FALSE);
    ASSERT_NE(nullptr, pabyData);
    std::string content(reinterpret_cast<char*>(pabyData), nSize);
    EXPECT_THAT(content, HasSubstr("Layer___1__bad_"));
    EXPECT_THAT(content, HasSubstr("<Folder><name>Layer2</name>"));
    // Schema for Layer2 written during destructor
    EXPECT_THAT(content, HasSubstr("<Schema name=\"Layer2\" id=\"Layer2\">"));
    EXPECT_THAT(content,
                HasSubstr("<SimpleData name=\"StrField\">Hello</SimpleData>"));
  }
}

}  // namespace
}  // namespace autotest2
