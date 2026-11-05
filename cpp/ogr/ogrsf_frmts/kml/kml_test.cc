// Copyright 2015 Google Inc. All Rights Reserved.
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
// Tests KML OGR driver (not based on libkml).
//
// See also:
//   https://gdal.org/drivers/vector/kml.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_kml.py

#include "third_party/gdal/ogr/ogrsf_frmts/kml/kml.h"

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlnode.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlutility.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlvector.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class KmlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRKML();
  }
};

TEST_F(KmlTest, Point) {
  constexpr char kPointKml[] = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
<Document>
  <Placemark>
    <name>Clipperton Island</name>
    <Point>
      <coordinates>-109.27,10.30</coordinates>
    </Point>
  </Placemark>
</Document>
</kml>)xml";

  constexpr char kPath[] = "/vsimem/kml_test/point.kml";
  VsiMemTempWrapper f(kPath, kPointKml);

  std::unique_ptr<GDALDataset> src(static_cast<GDALDataset*>(GDALOpenEx(
      kPath, GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));

  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  ASSERT_EQ(1, src->GetLayerCount());

  OGRLayer *layer = src->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbPoint, layer->GetGeomType());
  EXPECT_NE(wkbPoint25D, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());
  // "Layer #" plus the actual layer number is assigned to layers
  // without names by OGRKMLDataSource::Open.
  ASSERT_STREQ("Layer #0", layer->GetName());

  std::unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_STREQ("POINT (-109.27 10.3)", wkt.get());
}

TEST(KmlCoreTest, BaseKmlMethodsAndUnopened) {
  KML kml;
  EXPECT_FALSE(kml.isLeaf("name"));
  EXPECT_FALSE(kml.isFeature("Point"));
  EXPECT_FALSE(kml.isFeatureContainer("Placemark"));
  EXPECT_FALSE(kml.isContainer("Folder"));
  EXPECT_FALSE(kml.isRest("LinearRing"));
  EXPECT_FALSE(kml.isHandled("Point"));
  kml.findLayers(nullptr, 0);

  // Unopened state queries
  EXPECT_FALSE(kml.parse());
  EXPECT_EQ("No file given", kml.getError());
  EXPECT_FALSE(kml.isValid());
  EXPECT_EQ("No file given", kml.getError());
  EXPECT_EQ(0, kml.classifyNodes());
  kml.eliminateEmpty();
  kml.print(3);
  EXPECT_EQ(-1, kml.getNumLayers());
  EXPECT_FALSE(kml.selectLayer(0));
  EXPECT_FALSE(kml.selectLayer(-1));
  EXPECT_EQ("", kml.getCurrentName());
  EXPECT_EQ(OGRKML::Unknown, kml.getCurrentType());
  EXPECT_EQ(OGRKML::Unknown, kml.is25D());
  EXPECT_EQ(-1, kml.getNumFeatures());
  int last_asked = -1;
  int last_count = -1;
  EXPECT_EQ(nullptr, kml.getFeature(0, last_asked, last_count));

  // Open file, and re-open to trigger closing previous file handle
  constexpr char kPath1[] = "/vsimem/kml_core_test/f1.kml";
  constexpr char kPath2[] = "/vsimem/kml_core_test/f2.kml";
  VsiMemTempWrapper f1(kPath1, "<kml><Document/></kml>");
  VsiMemTempWrapper f2(kPath2, "<kml><Document/></kml>");
  EXPECT_TRUE(kml.open(kPath1));
  EXPECT_TRUE(kml.open(kPath2));

  // Parse on base KML: elements are unhandled, so endElement deletes the node
  // and resets poTrunk_ to nullptr.
  EXPECT_TRUE(kml.parse());
  EXPECT_EQ(0, kml.classifyNodes());
}

TEST(KmlCoreTest, CheckValidityVariations) {
  constexpr char kV22Earth[] = "/vsimem/kml_core_test/v22_earth.kml";
  VsiMemTempWrapper f_v22e(
      kV22Earth,
      "<kml xmlns=\"http://earth.google.com/kml/2.2\"><Document/></kml>");
  KMLVector k1;
  EXPECT_TRUE(k1.open(kV22Earth));
  EXPECT_TRUE(k1.isValid());

  constexpr char kV22Ogc[] = "/vsimem/kml_core_test/v22_ogc.kml";
  VsiMemTempWrapper f_v22o(
      kV22Ogc,
      "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document/></kml>");
  KMLVector k2;
  EXPECT_TRUE(k2.open(kV22Ogc));
  EXPECT_TRUE(k2.isValid());

  constexpr char kV21[] = "/vsimem/kml_core_test/v21.kml";
  VsiMemTempWrapper f_v21(
      kV21, "<kml xmlns=\"http://earth.google.com/kml/2.1\"><Document/></kml>");
  KMLVector k3;
  EXPECT_TRUE(k3.open(kV21));
  EXPECT_TRUE(k3.isValid());

  constexpr char kV20[] = "/vsimem/kml_core_test/v20.kml";
  VsiMemTempWrapper f_v20(
      kV20, "<kml xmlns=\"http://earth.google.com/kml/2.0\"><Document/></kml>");
  KMLVector k4;
  EXPECT_TRUE(k4.open(kV20));
  EXPECT_TRUE(k4.isValid());

  constexpr char kOtherXmlns[] = "/vsimem/kml_core_test/other_xmlns.kml";
  VsiMemTempWrapper f_other(
      kOtherXmlns, "<kml xmlns=\"http://example.com/kml\"><Document/></kml>");
  KMLVector k5;
  EXPECT_TRUE(k5.open(kOtherXmlns));
  EXPECT_TRUE(k5.isValid());

  constexpr char kNoXmlns[] = "/vsimem/kml_core_test/no_xmlns.kml";
  VsiMemTempWrapper f_noxmlns(kNoXmlns, "<kml id=\"1\"><Document/></kml>");
  KMLVector k6;
  EXPECT_TRUE(k6.open(kNoXmlns));
  EXPECT_TRUE(k6.isValid());

  constexpr char kPrefix[] = "/vsimem/kml_core_test/prefix.kml";
  VsiMemTempWrapper f_prefix(kPrefix,
                             "<kml:kml "
                             "xmlns:kml=\"http://earth.google.com/kml/"
                             "2.2\"><kml:Document/></kml:kml>");
  KMLVector k7;
  EXPECT_TRUE(k7.open(kPrefix));
  EXPECT_TRUE(k7.isValid());

  constexpr char kNotKml[] = "/vsimem/kml_core_test/not_kml.xml";
  VsiMemTempWrapper f_notkml(
      kNotKml, "<other xmlns=\"http://earth.google.com/kml/2.2\"/>");
  KMLVector k8;
  EXPECT_TRUE(k8.open(kNotKml));
  EXPECT_FALSE(k8.isValid());

  // Call parse() first, then isValid() to test deleting pre-existing poTrunk_
  KMLVector k_parse_then_valid;
  EXPECT_TRUE(k_parse_then_valid.open(kV22Earth));
  EXPECT_TRUE(k_parse_then_valid.parse());
  EXPECT_TRUE(k_parse_then_valid.isValid());

  // Invalid XML: short XML with <?xml and <kml
  constexpr char kBad1[] = "/vsimem/kml_core_test/bad1.kml";
  VsiMemTempWrapper f_bad1(kBad1, "<?xml version=\"1.0\"?><kml><broken");
  KMLVector k_bad1;
  EXPECT_TRUE(k_bad1.open(kBad1));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_bad1.isValid());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("XML parsing of KML file failed"));
  }

  // Invalid XML: large buffer (>= 8192 bytes) with <Document and /kml/2.
  std::string big_bad =
      "<?xml version=\"1.0\"?><Document "
      "xmlns=\"http://www.opengis.net/kml/2.2\"></mismatched>";
  big_bad.append(8200, ' ');
  constexpr char kBad2[] = "/vsimem/kml_core_test/bad2.kml";
  VsiMemTempWrapper f_bad2(kBad2, big_bad);
  KMLVector k_bad2;
  EXPECT_TRUE(k_bad2.open(kBad2));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_bad2.isValid());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("XML parsing of KML file failed"));
  }

  // Invalid XML without <?xml: fails silently without CPLError
  constexpr char kBad3[] = "/vsimem/kml_core_test/bad3.kml";
  VsiMemTempWrapper f_bad3(kBad3, "<kml><broken");
  KMLVector k_bad3;
  EXPECT_TRUE(k_bad3.open(kBad3));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_bad3.isValid());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }

  // 50-chunk loop limit without reaching valid/invalid conclusion
  std::string big_comment = "<!--";
  big_comment.append(50 * 8192, 'x');
  big_comment += "-->";
  constexpr char kCommentPath[] = "/vsimem/kml_core_test/comment.kml";
  VsiMemTempWrapper f_comment(kCommentPath, big_comment);
  KMLVector k_comment;
  EXPECT_TRUE(k_comment.open(kCommentPath));
  EXPECT_FALSE(k_comment.isValid());

  // Million laughs pattern in checkValidity (dataHandlerValidate)
  std::string laughs = R"xml(<?xml version="1.0"?>
<!DOCTYPE kml [
  <!ENTITY a "1234567890">
  <!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;">
  <!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;">
  <!ENTITY d "&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;">
]>
<kml xmlns="http://www.opengis.net/kml/2.2">&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;</kml>)xml";
  constexpr char kMlPath[] = "/vsimem/kml_core_test/ml.kml";
  VsiMemTempWrapper f_ml(kMlPath, laughs);
  KMLVector k_ml;
  EXPECT_TRUE(k_ml.open(kMlPath));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_ml.isValid());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("parsing aborted"));
    CPLErrorReset();
  }
}

TEST(KmlCoreTest, ParseVariationsAndErrorHandling) {
  constexpr char kV22Earth[] = "/vsimem/kml_core_test/v22_parse.kml";
  VsiMemTempWrapper f_v22e(
      kV22Earth,
      "<kml xmlns=\"http://earth.google.com/kml/2.2\"><Document/></kml>");
  KMLVector kml;
  EXPECT_TRUE(kml.open(kV22Earth));
  // Parse twice in a row to exercise poTrunk_ cleanup at start of parse()
  EXPECT_TRUE(kml.parse());
  EXPECT_TRUE(kml.parse());

  // Parsing error when poCurrent_ != nullptr (nested elements)
  constexpr char kPerr1[] = "/vsimem/kml_core_test/perr1.kml";
  VsiMemTempWrapper f_perr1(kPerr1,
                            "<kml><Document><Folder><Placemark><broken");
  KMLVector k_perr1;
  EXPECT_TRUE(k_perr1.open(kPerr1));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_perr1.parse());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Parsing error when poCurrent_ == nullptr (trailing garbage after root)
  constexpr char kPerr2[] = "/vsimem/kml_core_test/perr2.kml";
  VsiMemTempWrapper f_perr2(kPerr2,
                            "<kml><Document/></kml>trailing_broken<tag>");
  KMLVector k_perr2;
  EXPECT_TRUE(k_perr2.open(kPerr2));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_perr2.parse());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 10 chunks of whitespace without event: nWithoutEventCounter == 10
  std::string big_gap = "<kml ";
  big_gap.append(10 * 8192 + 100, ' ');
  big_gap += "></kml>";
  constexpr char kNoEvtPath[] = "/vsimem/kml_core_test/no_evt.kml";
  VsiMemTempWrapper f_no_evt(kNoEvtPath, big_gap);
  KMLVector k_no_evt;
  EXPECT_TRUE(k_no_evt.open(kNoEvtPath));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_no_evt.parse());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too much data"));
  }

  // Depth limit of 1024
  std::string deep = "<kml>";
  for (int i = 0; i < 1025; ++i) {
    deep += "<Folder>";
  }
  for (int i = 0; i < 1025; ++i) {
    deep += "</Folder>";
  }
  deep += "</kml>";
  constexpr char kDeepPath[] = "/vsimem/kml_core_test/deep.kml";
  VsiMemTempWrapper f_deep(kDeepPath, deep);
  KMLVector k_deep;
  EXPECT_TRUE(k_deep.open(kDeepPath));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_deep.parse());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("parsing aborted"));
  }

  // Million laughs pattern in parse (dataHandler)
  constexpr char kMlPath[] = "/vsimem/kml_core_test/ml2.kml";
  std::string laughs = R"xml(<?xml version="1.0"?>
<!DOCTYPE kml [
  <!ENTITY a "1234567890">
  <!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;">
  <!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;">
  <!ENTITY d "&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;">
]>
<kml xmlns="http://www.opengis.net/kml/2.2">&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;</kml>)xml";
  VsiMemTempWrapper f_ml(kMlPath, laughs);
  KMLVector k_ml_parse;
  EXPECT_TRUE(k_ml_parse.open(kMlPath));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(k_ml_parse.parse());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("parsing aborted"));
    CPLErrorReset();
  }
}

TEST(KmlCoreTest, DescriptionHtmlAndCoordinatesSplitting) {
  constexpr char kComplexKml[] =
      R"xml(<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Folder>
      <name>
        Multi-line
        Folder Name
      </name>
      <Placemark>
        <name>
          SingleLineName
        </name>
        <description><b class="heading">HTML</b> first tag and <a href="http://example.com" target="_blank">link</a></description>
        <Point>
          <coordinates>
            -122.08,37.42,10.0
             -122.09,37.43,20.0
          </coordinates>
        </Point>
      </Placemark>
      <Placemark>
        <name>EmptyCoords</name>
        <description>Text first <i id="1">italic</i> tail</description>
        <Point>
          <coordinates>

          </coordinates>
        </Point>
      </Placemark>
    </Folder>
  </Document>
</kml>)xml";

  constexpr char kPath[] = "/vsimem/kml_core_test/complex.kml";
  VsiMemTempWrapper f(kPath, kComplexKml);
  KMLVector kml;
  ASSERT_TRUE(kml.open(kPath));
  ASSERT_TRUE(kml.parse());
  ASSERT_EQ(TRUE, kml.classifyNodes());
  EXPECT_FALSE(kml.hasOnlyEmpty());
  kml.findLayers(nullptr, 0);
  ASSERT_EQ(1, kml.getNumLayers());
  ASSERT_TRUE(kml.selectLayer(0));

  // 1. Layer metadata: <Folder><name> has two indented non-empty lines, which
  // KML::endElement (kml.cpp:435-474) strips of leading whitespace and joins
  // with '\n'. Coordinate splitting into 3-tuple tokens classified the layer as
  // 2.5D Point.
  EXPECT_EQ("Multi-line\nFolder Name", kml.getCurrentName());
  EXPECT_EQ(OGRKML::Point, kml.getCurrentType());
  EXPECT_EQ(TRUE, kml.is25D());
  EXPECT_EQ(2, kml.getNumFeatures());

  int last_asked = -1;
  int last_count = -1;

  // 2. Feature 0:
  // - <name> has leading/trailing newlines and spaces, trimmed to single line.
  // - <description> starts with an HTML tag (numContent() == 0 -> addContent in
  //   KML::startElement; kml.cpp:272-273).
  // - <coordinates> contains two whitespace-separated tuples. KML::endElement
  //   splits them into distinct tokens (kml.cpp:383-421). KMLNode::getGeometry
  //   (kmlnode.cpp:505-516) consumes the first valid parsed coordinate
  //   (-122.08, 37.42, 10.0) for Point and ignores the second tuple.
  std::unique_ptr<OGRKML::Feature> feat0(
      kml.getFeature(0, last_asked, last_count));
  ASSERT_NE(nullptr, feat0);
  EXPECT_EQ(0, last_asked);
  EXPECT_EQ(1, last_count);
  EXPECT_EQ("SingleLineName", feat0->sName);
  EXPECT_EQ(
      "<b class=\"heading\">HTML</b> first tag and "
      "<a href=\"http://example.com\" target=\"_blank\">link</a>",
      feat0->sDescription);
  EXPECT_EQ(OGRKML::Point, feat0->eType);
  ASSERT_NE(nullptr, feat0->poGeom);
  ASSERT_EQ(wkbPoint25D, feat0->poGeom->getGeometryType());
  OGRPoint* pt0 = feat0->poGeom->toPoint();
  EXPECT_DOUBLE_EQ(-122.08, pt0->getX());
  EXPECT_DOUBLE_EQ(37.42, pt0->getY());
  EXPECT_DOUBLE_EQ(10.0, pt0->getZ());

  // 3. Feature 1:
  // - <description> starts with text before an HTML tag (numContent() == 0 in
  //   dataHandler, then numContent() > 0 -> appendContent in KML::startElement;
  //   kml.cpp:274-275).
  // - Whitespace-only <coordinates> is rejected by ParseCoordinate
  //   (kmlnode.cpp:75-79), falling back to an empty 2D OGRPoint
  //   (kmlnode.cpp:521).
  std::unique_ptr<OGRKML::Feature> feat1(
      kml.getFeature(1, last_asked, last_count));
  ASSERT_NE(nullptr, feat1);
  EXPECT_EQ(1, last_asked);
  EXPECT_EQ(2, last_count);
  EXPECT_EQ("EmptyCoords", feat1->sName);
  EXPECT_EQ("Text first <i id=\"1\">italic</i> tail", feat1->sDescription);
  EXPECT_EQ(OGRKML::Point, feat1->eType);
  ASSERT_NE(nullptr, feat1->poGeom);
  EXPECT_EQ(wkbPoint, feat1->poGeom->getGeometryType());
  EXPECT_TRUE(feat1->poGeom->IsEmpty());
}

TEST(KmlCoreTest, LayerSelectionAndUnregisterLayers) {
  constexpr char kMultiLayerKml[] =
      R"xml(<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Folder><name>Layer1</name>
      <Placemark><Point><coordinates>1,2</coordinates></Point></Placemark>
    </Folder>
    <Folder><name>Layer2</name>
      <Placemark><Point><coordinates>3,4</coordinates></Point></Placemark>
    </Folder>
  </Document>
</kml>)xml";

  constexpr char kPath[] = "/vsimem/kml_core_test/multilayer.kml";
  VsiMemTempWrapper f(kPath, kMultiLayerKml);
  KMLVector kml;
  EXPECT_TRUE(kml.open(kPath));
  EXPECT_TRUE(kml.parse());
  EXPECT_EQ(1, kml.classifyNodes());
  EXPECT_FALSE(kml.hasOnlyEmpty());
  kml.findLayers(nullptr, 0);

  EXPECT_EQ(2, kml.getNumLayers());

  EXPECT_TRUE(kml.selectLayer(0));
  EXPECT_EQ("Layer1", kml.getCurrentName());
  EXPECT_EQ(OGRKML::Point, kml.getCurrentType());
  EXPECT_EQ(0, kml.is25D());
  EXPECT_EQ(1, kml.getNumFeatures());

  int last_asked = -1;
  int last_count = -1;
  std::unique_ptr<OGRKML::Feature> feat(
      kml.getFeature(0, last_asked, last_count));
  ASSERT_NE(nullptr, feat);

  EXPECT_TRUE(kml.selectLayer(1));
  EXPECT_EQ("Layer2", kml.getCurrentName());

  // Test unregisterLayerIfMatchingThisNode
  // Unregister first layer: triggers memmove because i < nNumLayers_ - 1
  KMLNode dummy_node1;
  dummy_node1.setName("dummy1");
  KMLNode dummy_node2;
  dummy_node2.setName("dummy2");

  // Create an artificial KML subclass with dummy nodes to directly verify
  // unregister
  class TestKML : public KML {
   public:
    void SetLayers(KMLNode* n1, KMLNode* n2) {
      nNumLayers_ = 2;
      papoLayers_ = static_cast<KMLNode**>(CPLMalloc(2 * sizeof(KMLNode*)));
      papoLayers_[0] = n1;
      papoLayers_[1] = n2;
    }
  };

  TestKML test_kml;
  test_kml.SetLayers(&dummy_node1, &dummy_node2);
  EXPECT_EQ(2, test_kml.getNumLayers());
  // Unregister matching node 0: triggers memmove
  test_kml.unregisterLayerIfMatchingThisNode(&dummy_node1);
  EXPECT_EQ(1, test_kml.getNumLayers());
  // Unregister non-matching node: loop finishes with i++
  test_kml.unregisterLayerIfMatchingThisNode(&dummy_node1);
  EXPECT_EQ(1, test_kml.getNumLayers());
  // Unregister matching node 1: triggers unregister without memmove
  test_kml.unregisterLayerIfMatchingThisNode(&dummy_node2);
  EXPECT_EQ(0, test_kml.getNumLayers());
}

}  // namespace
}  // namespace autotest2
