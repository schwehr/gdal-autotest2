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
// Tests KMLVector (kmlvector.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/kml/kmlvector.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlvector.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlnode.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(KmlVectorTest, ElementRecognitionMethods) {
  KMLVector kv;

  // 1. isLeaf
  EXPECT_TRUE(kv.isLeaf("name"));
  EXPECT_TRUE(kv.isLeaf("coordinates"));
  EXPECT_TRUE(kv.isLeaf("altitudeMode"));
  EXPECT_TRUE(kv.isLeaf("description"));
  EXPECT_FALSE(kv.isLeaf("Folder"));
  EXPECT_FALSE(kv.isLeaf("unknown"));

  // 2. isContainer
  EXPECT_TRUE(kv.isContainer("Folder"));
  EXPECT_TRUE(kv.isContainer("Document"));
  EXPECT_TRUE(kv.isContainer("kml"));
  EXPECT_FALSE(kv.isContainer("Placemark"));
  EXPECT_FALSE(kv.isContainer("unknown"));

  // 3. isFeatureContainer
  EXPECT_TRUE(kv.isFeatureContainer("MultiGeometry"));
  EXPECT_TRUE(kv.isFeatureContainer("MultiPolygon"));
  EXPECT_TRUE(kv.isFeatureContainer("MultiLineString"));
  EXPECT_TRUE(kv.isFeatureContainer("MultiPoint"));
  EXPECT_TRUE(kv.isFeatureContainer("Placemark"));
  EXPECT_FALSE(kv.isFeatureContainer("Folder"));
  EXPECT_FALSE(kv.isFeatureContainer("unknown"));

  // 4. isFeature
  EXPECT_TRUE(kv.isFeature("Polygon"));
  EXPECT_TRUE(kv.isFeature("LineString"));
  EXPECT_TRUE(kv.isFeature("Point"));
  EXPECT_FALSE(kv.isFeature("Placemark"));
  EXPECT_FALSE(kv.isFeature("unknown"));

  // 5. isRest
  EXPECT_TRUE(kv.isRest("outerBoundaryIs"));
  EXPECT_TRUE(kv.isRest("innerBoundaryIs"));
  EXPECT_TRUE(kv.isRest("LinearRing"));
  EXPECT_FALSE(kv.isRest("coordinates"));
  EXPECT_FALSE(kv.isRest("unknown"));
}

TEST(KmlVectorTest, FindLayersBranches) {
  // 1. Feature at trunk -> returns early
  constexpr char kPointKml[] = "<Point><coordinates>1,2</coordinates></Point>";
  VsiMemTempWrapper f_pt("/vsimem/kmlvector_test/pt.kml", kPointKml);
  KMLVector kv_pt;
  EXPECT_TRUE(kv_pt.open("/vsimem/kmlvector_test/pt.kml"));
  EXPECT_TRUE(kv_pt.parse());
  kv_pt.findLayers(nullptr, 0);
  EXPECT_EQ(0, kv_pt.getNumLayers());

  // 2. FeatureContainer at trunk -> returns early
  constexpr char kPmKml[] =
      "<Placemark><Point><coordinates>1,2</coordinates></Point></Placemark>";
  VsiMemTempWrapper f_pm("/vsimem/kmlvector_test/pm.kml", kPmKml);
  KMLVector kv_pm;
  EXPECT_TRUE(kv_pm.open("/vsimem/kmlvector_test/pm.kml"));
  EXPECT_TRUE(kv_pm.parse());
  kv_pm.findLayers(nullptr, 0);
  EXPECT_EQ(0, kv_pm.getNumLayers());

  // 3. Rest node at trunk -> returns early
  constexpr char kRingKml[] =
      "<LinearRing><coordinates>0,0 1,1 0,0</coordinates></LinearRing>";
  VsiMemTempWrapper f_ring("/vsimem/kmlvector_test/ring.kml", kRingKml);
  KMLVector kv_ring;
  EXPECT_TRUE(kv_ring.open("/vsimem/kmlvector_test/ring.kml"));
  EXPECT_TRUE(kv_ring.parse());
  kv_ring.findLayers(nullptr, 0);
  EXPECT_EQ(0, kv_ring.getNumLayers());

  // 4. Empty containers without bKeepEmptyContainers
  constexpr char kEmptyKml[] =
      "<kml><Document><Folder><name>OuterEmpty</name></Folder></Document></"
      "kml>";
  VsiMemTempWrapper f_empty("/vsimem/kmlvector_test/empty.kml", kEmptyKml);
  KMLVector kv_empty;
  EXPECT_TRUE(kv_empty.open("/vsimem/kmlvector_test/empty.kml"));
  EXPECT_TRUE(kv_empty.parse());
  kv_empty.findLayers(nullptr, 0);
  EXPECT_EQ(0, kv_empty.getNumLayers());

  // 5. Container with bKeepEmptyContainers = true and mixed empty / non-empty
  constexpr char kMixedEmptyKml[] = R"xml(<kml><Document>
    <Folder><name>ParentFolder</name>
      <Folder><name>EmptySubFolder</name></Folder>
      <Placemark><Point><coordinates>1,2</coordinates></Point></Placemark>
    </Folder>
  </Document></kml>)xml";
  VsiMemTempWrapper f_mempty("/vsimem/kmlvector_test/mempty.kml",
                             kMixedEmptyKml);
  KMLVector kv_mempty;
  EXPECT_TRUE(kv_mempty.open("/vsimem/kmlvector_test/mempty.kml"));
  EXPECT_TRUE(kv_mempty.parse());
  EXPECT_EQ(1, kv_mempty.classifyNodes());
  kv_mempty.findLayers(nullptr, 1);
  EXPECT_EQ(1, kv_mempty.getNumLayers());

  // 6. Unclassified container: strange type branch
  constexpr char kUnclassKml[] = R"xml(<kml><Document>
    <Folder><name>UnclassifiedFolder</name>
      <Placemark><Point><coordinates>1,2</coordinates></Point></Placemark>
    </Folder>
  </Document></kml>)xml";
  VsiMemTempWrapper f_unclass("/vsimem/kmlvector_test/unclass.kml",
                              kUnclassKml);
  KMLVector kv_unclass;
  EXPECT_TRUE(kv_unclass.open("/vsimem/kmlvector_test/unclass.kml"));
  EXPECT_TRUE(kv_unclass.parse());
  // Do NOT call classifyNodes() so nodeType remains Unknown
  kv_unclass.findLayers(nullptr, 0);
  EXPECT_EQ(0, kv_unclass.getNumLayers());

  // 7. Unknown node passed directly: exercises debug branch when KML_DEBUG is
  // set
  KMLNode custom_node;
  custom_node.setName("CustomNode");
  {
    CPLConfigOptionSetter debug_setter("KML_DEBUG", "ON", false);
    kv_mempty.findLayers(&custom_node, 0);
  }
}

TEST(KmlVectorTest, FullParsingAndFindLayers) {
  constexpr char kKml[] = R"xml(<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Folder><name>Points</name>
      <Placemark><Point><coordinates>1,2</coordinates></Point></Placemark>
    </Folder>
    <Folder><name>Lines</name>
      <Placemark><LineString><coordinates>1,2 3,4</coordinates></LineString></Placemark>
    </Folder>
    <Folder><name>Polygons</name>
      <Placemark><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 1,0 1,1 0,1 0,0</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark>
    </Folder>
    <Folder><name>EmptyFolder</name>
    </Folder>
  </Document>
</kml>)xml";

  constexpr char kPath[] = "/vsimem/kmlvector_test/sample.kml";
  VsiMemTempWrapper f(kPath, kKml);
  KMLVector kv;
  EXPECT_TRUE(kv.open(kPath));
  EXPECT_TRUE(kv.isValid());
  EXPECT_TRUE(kv.parse());
  EXPECT_EQ(1, kv.classifyNodes());
  EXPECT_FALSE(kv.hasOnlyEmpty());

  // findLayers with nullptr starts with trunk
  kv.findLayers(nullptr, 0);
  EXPECT_EQ(3, kv.getNumLayers());
}

}  // namespace
}  // namespace autotest2
