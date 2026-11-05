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
// Tests for OGRGMLDataSource (ogrgmldatasource.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/ogrgmldatasource.cpp

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/ogr_gml.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class OGRGMLDataSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGML();
    CPLSetConfigOption("CPL_TMPDIR", "/vsimem");
  }

  void TearDown() override { CPLSetConfigOption("CPL_TMPDIR", nullptr); }
};

TEST_F(OGRGMLDataSourceTest, CheckHeaderValidation) {
  // Positive checks:
  EXPECT_TRUE(OGRGMLDataSource::CheckHeader("<wfs:FeatureCollection "));
  EXPECT_TRUE(OGRGMLDataSource::CheckHeader(
      "<root xmlns:gml=\"http://www.opengis.net/gml\">"));
  EXPECT_TRUE(OGRGMLDataSource::CheckHeader(
      "<root xmlns:gml='http://www.opengis.net/gml'>"));
  EXPECT_TRUE(OGRGMLDataSource::CheckHeader("<csw:GetRecordsResponse>"));

  // Negative checks:
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader("<root><item>val</item></root>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<kml xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<schema xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<xs:schema xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<xsd:schema xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<rss xmlns:georss=\"http://www.georss.org/georss\" "
      "xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<JCSDataFile xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<OGRWFSDataSource><root "
      "xmlns:gml=\"http://www.opengis.net/gml\"/></OGRWFSDataSource>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "<wfs:WFS_Capabilities xmlns:gml=\"http://www.opengis.net/gml\"/>"));
  EXPECT_FALSE(OGRGMLDataSource::CheckHeader(
      "http://www.opengis.net/wmts/1.0 "
      "xmlns:gml=\"http://www.opengis.net/gml\""));
}

TEST_F(OGRGMLDataSourceTest, OpenInvalidAndEmptyFiles) {
  WithQuietHandler quiet_handler;
  auto open_info_nonexist = std::make_unique<GDALOpenInfo>(
      "/vsimem/ogrgmlds_test/non_existent.gml", GDAL_OF_READONLY, nullptr);
  auto ds = std::make_unique<OGRGMLDataSource>();
  EXPECT_FALSE(ds->Open(open_info_nonexist.get()));

  // Empty file:
  constexpr char kEmptyPath[] = "/vsimem/ogrgmlds_test/empty.gml";
  VsiMemTempWrapper f_empty(kEmptyPath, "");
  auto open_info_empty =
      std::make_unique<GDALOpenInfo>(kEmptyPath, GDAL_OF_READONLY, nullptr);
  ds = std::make_unique<OGRGMLDataSource>();
  EXPECT_FALSE(ds->Open(open_info_empty.get()));

  // Non-GML XML:
  constexpr char kNonGmlPath[] = "/vsimem/ogrgmlds_test/nongml.xml";
  VsiMemTempWrapper f_nongml(kNonGmlPath, "<root><item>val</item></root>");
  auto open_info_nongml =
      std::make_unique<GDALOpenInfo>(kNonGmlPath, GDAL_OF_READONLY, nullptr);
  ds = std::make_unique<OGRGMLDataSource>();
  EXPECT_FALSE(ds->Open(open_info_nongml.get()));
}

TEST_F(OGRGMLDataSourceTest, OpenBOMAndEncodingsAndParserConfig) {
  // 1. UTF-8 BOM prefix:
  {
    constexpr char kBomPath[] = "/vsimem/ogrgmlds_test/bom.gml";
    std::string bom_content =
        "\xEF\xBB\xBF<root "
        "xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
        "<Item "
        "fid=\"item.1\"><prop>val</prop><gml:Point><gml:coordinates>1,2</"
        "gml:coordinates></gml:Point>"
        "</Item></gml:featureMember></root>";
    VsiMemTempWrapper f_bom(kBomPath, bom_content);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kBomPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 2. Encodings: ISO-8859-1, ISO-8859-15, and UTF-8:
  {
    constexpr char kIsoPath[] = "/vsimem/ogrgmlds_test/iso.gml";
    std::string iso_content =
        "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n"
        "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
        "<Item "
        "fid=\"item.1\"><prop>caf\xe9</prop><gml:Point><gml:coordinates>1,2</"
        "gml:coordinates></gml:Point>"
        "</Item></gml:featureMember></root>";
    VsiMemTempWrapper f_iso(kIsoPath, iso_content);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kIsoPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  {
    constexpr char kIso15Path[] = "/vsimem/ogrgmlds_test/iso15.gml";
    std::string iso15_content =
        "<?xml version=\"1.0\" encoding=\"ISO-8859-15\"?>\n"
        "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
        "<Item "
        "fid=\"item.1\"><prop>val15</prop><gml:Point><gml:coordinates>1,2</"
        "gml:coordinates></gml:Point>"
        "</Item></gml:featureMember></root>";
    VsiMemTempWrapper f_iso15(kIso15Path, iso15_content);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kIso15Path, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 3. Parser config options GML_PARSER=EXPAT and GML_PARSER=XERCES:
  {
    constexpr char kParserPath[] = "/vsimem/ogrgmlds_test/parser.gml";
    std::string parser_content =
        "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
        "<Item "
        "fid=\"item.1\"><prop>val</prop><gml:Point><gml:coordinates>1,2</"
        "gml:coordinates></gml:Point>"
        "</Item></gml:featureMember></root>";
    VsiMemTempWrapper f_parser(kParserPath, parser_content);

    CPLSetConfigOption("GML_PARSER", "EXPAT");
    absl::Cleanup reset_parser = [] {
      CPLSetConfigOption("GML_PARSER", nullptr);
    };
    std::unique_ptr<GDALDataset> dsExpat(
        GDALDataset::Open(kParserPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    EXPECT_NE(nullptr, dsExpat);

    CPLSetConfigOption("GML_PARSER", "XERCES");
    WithQuietHandler quiet_handler;
    // XERCES may not be built-in, but tests the GML_PARSER=XERCES configuration
    // branch:
    std::unique_ptr<GDALDataset> dsXerces(
        GDALDataset::Open(kParserPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  }
}

TEST_F(OGRGMLDataSourceTest, OpenTransparentGzip) {
  constexpr char kUnderlyingPath[] = "/vsimem/ogrgmlds_test/compressed.gml.gz";
  VsiMemTempWrapper f_gz(kUnderlyingPath, "");
  std::string content =
      "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
      "<Item "
      "fid=\"item.1\"><prop>zipped</prop><gml:Point><gml:coordinates>5,10</"
      "gml:coordinates></gml:Point>"
      "</Item></gml:featureMember></root>";
  VSILFILE* fp =
      VSIFOpenL("/vsigzip//vsimem/ogrgmlds_test/compressed.gml.gz", "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Open without /vsigzip/ prefix:
  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(kUnderlyingPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("zipped", feat->GetFieldAsString("prop"));
}

TEST_F(OGRGMLDataSourceTest, OpenConnectionStringsAndOpenOptions) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/opts.gml";
  constexpr char kXsdPath[] = "/vsimem/ogrgmlds_test/opts.xsd";
  constexpr char kGmlContent[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
      "<gml:featureMember><Item "
      "fid=\"item.1\"><prop1>abc</prop1><prop2></prop2>"
      "<gml:Point "
      "srsName=\"EPSG:4326\"><gml:coordinates>10,20</gml:coordinates></"
      "gml:Point>"
      "</Item></gml:featureMember></root>";
  constexpr char kXsdContent[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "elementFormDefault=\"qualified\">"
      "<xs:import namespace=\"http://www.opengis.net/gml\"/>"
      "<xs:element name=\"Item\" type=\"ItemType\" "
      "substitutionGroup=\"gml:_Feature\"/>"
      "<xs:complexType name=\"ItemType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">"
      "<xs:sequence>"
      "<xs:element name=\"geometryProperty\" type=\"gml:PointPropertyType\"/>"
      "<xs:element name=\"prop1\" type=\"xs:string\"/>"
      "<xs:element name=\"prop2\" type=\"xs:string\" nillable=\"true\" "
      "minOccurs=\"0\"/>"
      "</xs:sequence></xs:extension></xs:complexContent></xs:complexType></"
      "xs:schema>";

  VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);
  VsiMemTempWrapper f_xsd(kXsdPath, kXsdContent);

  // 1. Connection string with ,xsd= suffix:
  std::string connStr = std::string(kGmlPath) + ",xsd=" + kXsdPath;
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(connStr.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 2. Open options: XSD, READ_MODE, EXPOSE_GML_ID, EXPOSE_FID, EMPTY_AS_NULL,
  // etc.
  {
    const char* const open_opts[] = {"XSD=/vsimem/ogrgmlds_test/opts.xsd",
                                     "READ_MODE=STANDARD",
                                     "EXPOSE_GML_ID=YES",
                                     "EXPOSE_FID=NO",
                                     "CONSIDER_EPSG_AS_URN=YES",
                                     "SWAP_COORDINATES=NO",
                                     "INVERT_AXIS_ORDER_IF_LAT_LONG=NO",
                                     "EMPTY_AS_NULL=YES",
                                     "GML_ATTRIBUTES_TO_OGR_FIELDS=YES",
                                     "USE_BBOX=YES",
                                     nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_NE(-1, layer->GetLayerDefn()->GetFieldIndex("gml_id"));
    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_STREQ("item.1", feat->GetFieldAsString("gml_id"));
    EXPECT_FALSE(feat->IsFieldSet(feat->GetFieldIndex("prop2")));
  }

  // 3. Alternative option values: READ_MODE=SEQUENTIAL_LAYERS, EXPOSE_FID=YES:
  {
    const char* const open_opts[] = {"READ_MODE=SEQUENTIAL_LAYERS",
                                     "EXPOSE_GML_ID=NO",
                                     "EXPOSE_FID=YES",
                                     "CONSIDER_EPSG_AS_URN=AUTO",
                                     "SWAP_COORDINATES=AUTO",
                                     "EMPTY_AS_NULL=NO",
                                     nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_NE(-1, layer->GetLayerDefn()->GetFieldIndex("fid"));
  }

  // 4. READ_MODE=INTERLEAVED_LAYERS and invalid READ_MODE:
  {
    const char* const open_opts[] = {"READ_MODE=INTERLEAVED_LAYERS", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    EXPECT_NE(nullptr, ds);
  }
  {
    const char* const open_opts[] = {"READ_MODE=INVALID_MODE", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    EXPECT_NE(nullptr, ds);
  }
}

TEST_F(OGRGMLDataSourceTest, FindAndParseTopElementsAndStandaloneGeometry) {
  // 1. Standalone geometry:
  constexpr char kGeomPath[] = "/vsimem/ogrgmlds_test/standalone.gml";
  constexpr char kGeomContent[] =
      "<gml:Point xmlns:gml=\"http://www.opengis.net/gml\" "
      "srsName=\"EPSG:4326\">"
      "<gml:coordinates>10,20</gml:coordinates></gml:Point>";
  VsiMemTempWrapper f_geom(kGeomPath, kGeomContent);

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(kGeomPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  ASSERT_EQ(1, ds->GetLayerCount());

  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_STREQ("geometry", layer->GetName());
  EXPECT_EQ(wkbPoint, wkbFlatten(layer->GetGeomType()));

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  ASSERT_NE(nullptr, feat->GetGeometryRef());
  EXPECT_DOUBLE_EQ(10.0, feat->GetGeometryRef()->toPoint()->getX());
  EXPECT_DOUBLE_EQ(20.0, feat->GetGeometryRef()->toPoint()->getY());
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // 2. Top-level description and name metadata:
  constexpr char kMetaPath[] = "/vsimem/ogrgmlds_test/metadata.gml";
  constexpr char kMetaContent[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\">"
      "<gml:description>Sample &lt;Description&gt;</gml:description>"
      "<gml:name>SampleName</gml:name>"
      "<gml:boundedBy><gml:Envelope "
      "srsName=\"http://www.opengis.net/gml/srs/epsg.xml#4326\">"
      "<gml:lowerCorner>10 20</gml:lowerCorner><gml:upperCorner>30 "
      "40</gml:upperCorner>"
      "</gml:Envelope></gml:boundedBy>"
      "<gml:featureMember><Item fid=\"item.1\">"
      "<gml:description>Item Desc</gml:description>"
      "<gml:identifier>urn:id:1</gml:identifier>"
      "<gml:name>Item Name</gml:name>"
      "<gml:Point><gml:coordinates>15,25</gml:coordinates></gml:Point>"
      "</Item></gml:featureMember></root>";
  VsiMemTempWrapper f_meta(kMetaPath, kMetaContent);

  std::unique_ptr<GDALDataset> dsMeta(
      GDALDataset::Open(kMetaPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, dsMeta);
  EXPECT_STREQ("Sample <Description>", dsMeta->GetMetadataItem("DESCRIPTION"));
  EXPECT_STREQ("SampleName", dsMeta->GetMetadataItem("NAME"));
}

TEST_F(OGRGMLDataSourceTest, SpecialProfilesWFS_MTKGML_FME_FGD) {
  // 1. WFS FeatureCollection with numberOfFeatures and numberReturned:
  {
    constexpr char kWfsPath[] = "/vsimem/ogrgmlds_test/wfs_fc.gml";
    constexpr char kWfsContent[] =
        "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
        "xmlns:gml=\"http://www.opengis.net/gml\" numberOfFeatures=\"1\">"
        "<wfs:boundedBy><gml:Envelope srsName=\"EPSG:4326\"><gml:lowerCorner>1 "
        "2</gml:lowerCorner>"
        "<gml:upperCorner>3 4</gml:upperCorner></gml:Envelope></wfs:boundedBy>"
        "<gml:featureMember><LayerA gml:id=\"a.1\"><prop>valA</prop>"
        "<gml:Point><gml:coordinates>2,3</gml:coordinates></gml:Point></"
        "LayerA></gml:featureMember>"
        "</wfs:FeatureCollection>";
    VsiMemTempWrapper f_wfs(kWfsPath, kWfsContent);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kWfsPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* l = ds->GetLayer(0);
    ASSERT_NE(nullptr, l);
    EXPECT_EQ(1, l->GetFeatureCount(/*bForce=*/FALSE));
  }

  // 2. Deegree WFS FeatureCollection:
  {
    constexpr char kDeegreePath[] = "/vsimem/ogrgmlds_test/deegree.gml";
    constexpr char kDeegreeContent[] =
        "<FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
        "xmlns:gml=\"http://www.opengis.net/gml\">"
        "<gml:featureMember><Item fid=\"item.1\"><prop>val</prop>"
        "<gml:Point><gml:coordinates>1,1</gml:coordinates></gml:Point></Item></"
        "gml:featureMember>"
        "</FeatureCollection>";
    VsiMemTempWrapper f_deegree(kDeegreePath, kDeegreeContent);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kDeegreePath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 3. MTKGML profile (<Maastotiedot>):
  {
    constexpr char kMtkPath[] = "/vsimem/ogrgmlds_test/mtk.gml";
    constexpr char kMtkContent[] =
        "<Maastotiedot xmlns:gml=\"http://www.opengis.net/gml\" "
        "srsName=\"EPSG:3067\">"
        "<gml:featureMember><Kohteet "
        "fid=\"k.1\"><gml:Point><gml:coordinates>100,200</gml:coordinates></"
        "gml:Point>"
        "</Kohteet></gml:featureMember></Maastotiedot>";
    VsiMemTempWrapper f_mtk(kMtkPath, kMtkContent);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kMtkPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 4. Japan FGD profile:
  {
    constexpr char kFgdPath[] = "/vsimem/ogrgmlds_test/fgd.gml";
    constexpr char kFgdContent[] =
        "<root xmlns:gml=\"http://www.opengis.net/gml\" "
        "xmlns=\"http://fgd.gsi.go.jp/spec/2008/FGD_GMLSchema\" "
        "srsName=\"EPSG:4326\">"
        "<gml:featureMember><Item "
        "fid=\"f.1\"><gml:Point><gml:coordinates>139,35</gml:coordinates></"
        "gml:Point>"
        "</Item></gml:featureMember></root>";
    VsiMemTempWrapper f_fgd(kFgdPath, kFgdContent);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kFgdPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 5. FME hint (xmlns:fme):
  {
    constexpr char kFmePath[] = "/vsimem/ogrgmlds_test/fme.gml";
    constexpr char kFmeContent[] =
        "<root xmlns:gml=\"http://www.opengis.net/gml\" "
        "xmlns:fme=\"http://www.safe.com/gml/fme\">"
        "<gml:featureMember><Item "
        "fid=\"fme.1\"><gml:Point><gml:coordinates>10,20</gml:coordinates></"
        "gml:Point>"
        "</Item></gml:featureMember></root>";
    VsiMemTempWrapper f_fme(kFmePath, kFmeContent);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kFmePath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }
}

TEST_F(OGRGMLDataSourceTest, XLinkResolutionAndSkipOptionsAndGFS) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/xlink.gml";
  constexpr char kGmlContent[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
      "<gml:featureMember><Item fid=\"item.1\"><ref xlink:href=\"#target1\"/>"
      "<gml:Point><gml:coordinates>1,1</gml:coordinates></gml:Point></Item></"
      "gml:featureMember>"
      "<Target gml:id=\"target1\"><name>T1</name></Target>"
      "</root>";
  VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);

  // 1. SKIP_RESOLVE_ELEMS=ALL:
  {
    const char* const open_opts[] = {"SKIP_RESOLVE_ELEMS=ALL", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
  }

  // 2. GML_SAVE_RESOLVED_TO=SAME:
  {
    CPLSetConfigOption("GML_SAVE_RESOLVED_TO", "SAME");
    CPLSetConfigOption("GML_SKIP_RESOLVE_ELEMS", "NONE");
    absl::Cleanup reset_opts = [] {
      CPLSetConfigOption("GML_SAVE_RESOLVED_TO", nullptr);
      CPLSetConfigOption("GML_SKIP_RESOLVE_ELEMS", nullptr);
    };
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
  }

  // 3. Custom GML_SAVE_RESOLVED_TO ending with .gml and pre-existing
  // .resolved.gml:
  {
    constexpr char kResolvedPath[] =
        "/vsimem/ogrgmlds_test/custom_resolved.gml";
    VsiMemTempWrapper f_res(kResolvedPath, kGmlContent);
    CPLSetConfigOption("GML_SAVE_RESOLVED_TO", kResolvedPath);
    CPLSetConfigOption("GML_SKIP_RESOLVE_ELEMS", "NONE");
    absl::Cleanup reset_opts = [] {
      CPLSetConfigOption("GML_SAVE_RESOLVED_TO", nullptr);
      CPLSetConfigOption("GML_SKIP_RESOLVE_ELEMS", nullptr);
    };
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
  }

  // 4. SKIP_RESOLVE_ELEMS=HUGE (returns nullptr since SQLite is not built-in):
  {
    WithQuietHandler quiet_handler;
    const char* const open_opts[] = {"SKIP_RESOLVE_ELEMS=HUGE", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    EXPECT_EQ(nullptr, ds);
  }
}

TEST_F(OGRGMLDataSourceTest, GFSTemplateAndSidecarGFSAndRegistry) {
  // 1. GFS_TEMPLATE open option:
  constexpr char kTemplateGmlPath[] = "/vsimem/ogrgmlds_test/template_test.gml";
  constexpr char kTemplateGfsPath[] = "/vsimem/ogrgmlds_test/template_test.gfs";
  constexpr char kTemplateGml[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\">"
      "<gml:featureMember><LayerA fid=\"a.1\"><propA>valA</propA>"
      "<gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point></LayerA></"
      "gml:featureMember>"
      "</root>";
  constexpr char kTemplateGfs[] =
      "<GMLFeatureClassList><GMLFeatureClass><Name>LayerA</"
      "Name><ElementPath>LayerA</ElementPath>"
      "<PropertyDefn><Name>propA</Name><ElementPath>propA</"
      "ElementPath><Type>String</Type></PropertyDefn>"
      "<GeomPropertyDefn><Name>geom</Name><ElementPath></"
      "ElementPath><Type>Point</Type></GeomPropertyDefn>"
      "</GMLFeatureClass></GMLFeatureClassList>";
  VsiMemTempWrapper f_gml(kTemplateGmlPath, kTemplateGml);
  VsiMemTempWrapper f_gfs(kTemplateGfsPath, kTemplateGfs);

  {
    const char* const open_opts[] = {
        "GFS_TEMPLATE=/vsimem/ogrgmlds_test/template_test.gfs", nullptr};
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kTemplateGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
  }

  // 2. WRITE_GFS option:
  {
    constexpr char kWriteGfsPath[] = "/vsimem/ogrgmlds_test/writegfs.gml";
    VsiMemTempWrapper f_wgfs(kWriteGfsPath, kTemplateGml);
    const char* const open_opts[] = {"WRITE_GFS=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kWriteGfsPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL("/vsimem/ogrgmlds_test/writegfs.gfs", &sStat));
    VSIUnlink("/vsimem/ogrgmlds_test/writegfs.gfs");
  }

  // 3. REGISTRY open option:
  constexpr char kRegGmlPath[] = "/vsimem/ogrgmlds_test/reg_test.gml";
  constexpr char kRegGfsPath[] = "/vsimem/ogrgmlds_test/reg_test.gfs";
  constexpr char kRegXmlPath[] = "/vsimem/ogrgmlds_test/reg.xml";
  constexpr char kRegGml[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\" "
      "xmlns:ns=\"http://example.com/ns\">"
      "<gml:featureMember><ns:FeatureA fid=\"a.1\"><ns:prop>data</ns:prop>"
      "<gml:Point><gml:coordinates>10,20</gml:coordinates></gml:Point></"
      "ns:FeatureA></gml:featureMember>"
      "</root>";
  constexpr char kRegGfs[] =
      "<GMLFeatureClassList><GMLFeatureClass><Name>FeatureA</"
      "Name><ElementPath>FeatureA</ElementPath>"
      "<PropertyDefn><Name>prop</Name><ElementPath>prop</"
      "ElementPath><Type>String</Type></PropertyDefn>"
      "<GeomPropertyDefn><Name>geom</Name><ElementPath></"
      "ElementPath><Type>Point</Type></GeomPropertyDefn>"
      "</GMLFeatureClass></GMLFeatureClassList>";
  constexpr char kRegXml[] =
      "<gml_registry><namespace prefix=\"ns\" uri=\"http://example.com/ns\">"
      "<featureType elementName=\"FeatureA\" "
      "gfsSchemaLocation=\"/vsimem/ogrgmlds_test/reg_test.gfs\"/>"
      "</namespace></gml_registry>";
  VsiMemTempWrapper f_reg_gml(kRegGmlPath, kRegGml);
  VsiMemTempWrapper f_reg_gfs(kRegGfsPath, kRegGfs);
  VsiMemTempWrapper f_reg_xml(kRegXmlPath, kRegXml);

  {
    const char* const open_opts[] = {"REGISTRY=/vsimem/ogrgmlds_test/reg.xml",
                                     "FORCE_SRS_DETECTION=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kRegGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetLayerByName("FeatureA"));
  }
}

TEST_F(OGRGMLDataSourceTest, WFSJointLayersWithAndWithoutXSD) {
  // 1. Joint layer with <wfs:Tuple> without XSD
  // (BuildJointClassFromScannedSchema):
  constexpr char kTupleGmlPath[] = "/vsimem/ogrgmlds_test/tuple.gml";
  constexpr char kTupleGml[] =
      "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
      "xmlns:gml=\"http://www.opengis.net/gml\">"
      "<gml:featureMember><wfs:Tuple>"
      "<wfs:member><LayerA "
      "fid=\"a.1\"><propA>foo</propA><gml:Point><gml:coordinates>1,1</"
      "gml:coordinates></gml:Point></LayerA></wfs:member>"
      "<wfs:member><LayerB "
      "fid=\"b.1\"><propB>bar</propB><gml:Point><gml:coordinates>2,2</"
      "gml:coordinates></gml:Point></LayerB></wfs:member>"
      "</wfs:Tuple></gml:featureMember></wfs:FeatureCollection>";
  VsiMemTempWrapper f_tuple(kTupleGmlPath, kTupleGml);

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kTupleGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_STREQ("join_LayerA_LayerB", layer->GetName());
  }

  // 2. Joint layer with XSD (BuildJointClassFromXSD):
  constexpr char kTupleXsdPath[] = "/vsimem/ogrgmlds_test/tuple.xsd";
  constexpr char kTupleXsd[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "elementFormDefault=\"qualified\">"
      "<xs:import namespace=\"http://www.opengis.net/gml\"/>"
      "<xs:element name=\"LayerA\" type=\"LayerAType\" "
      "substitutionGroup=\"gml:_Feature\"/>"
      "<xs:complexType name=\"LayerAType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">"
      "<xs:sequence><xs:element name=\"geometryProperty\" "
      "type=\"gml:PointPropertyType\"/>"
      "<xs:element name=\"propA\" "
      "type=\"xs:string\"/></xs:sequence></xs:extension></xs:complexContent></"
      "xs:complexType>"
      "<xs:element name=\"LayerB\" type=\"LayerBType\" "
      "substitutionGroup=\"gml:_Feature\"/>"
      "<xs:complexType name=\"LayerBType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">"
      "<xs:sequence><xs:element name=\"geometryProperty\" "
      "type=\"gml:PointPropertyType\"/>"
      "<xs:element name=\"propB\" "
      "type=\"xs:string\"/></xs:sequence></xs:extension></xs:complexContent></"
      "xs:complexType>"
      "</xs:schema>";
  VsiMemTempWrapper f_tuple_xsd(kTupleXsdPath, kTupleXsd);

  {
    const char* const open_opts[] = {"XSD=/vsimem/ogrgmlds_test/tuple.xsd",
                                     nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kTupleGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_STREQ("join_LayerA_LayerB", layer->GetName());
  }
}

TEST_F(OGRGMLDataSourceTest, TranslateGMLSchemaCompoundCRSAndPatchGFS) {
  // 1. Compound CRS with GML_REPORT_COMPD_CS=TRUE vs FALSE:
  constexpr char kCompdGmlPath[] = "/vsimem/ogrgmlds_test/compd.gml";
  constexpr char kCompdGfsPath[] = "/vsimem/ogrgmlds_test/compd.gfs";
  constexpr char kCompdGml[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
      "<Item fid=\"item.1\"><prop>val</prop>"
      "<lod2Solid><gml:Point><gml:coordinates>10,20,30</gml:coordinates></"
      "gml:Point></lod2Solid>"
      "</Item></gml:featureMember></root>";
  constexpr char kCompdGfs[] =
      "<GMLFeatureClassList><GMLFeatureClass><Name>Item</"
      "Name><ElementPath>Item</ElementPath>"
      "<SRSName>EPSG:7415</SRSName>"
      "<PropertyDefn><Name>prop</Name><ElementPath>prop</"
      "ElementPath><Type>String</Type></PropertyDefn>"
      "<GeomPropertyDefn><Name>lod2Solid</Name><ElementPath>lod2Solid</"
      "ElementPath><Type>PolyhedralSurface</Type></GeomPropertyDefn>"
      "</GMLFeatureClass></GMLFeatureClassList>";
  VsiMemTempWrapper f_gml(kCompdGmlPath, kCompdGml);
  VsiMemTempWrapper f_gfs(kCompdGfsPath, kCompdGfs);

  {
    CPLSetConfigOption("GML_REPORT_COMPD_CS", "FALSE");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GML_REPORT_COMPD_CS", nullptr);
    };
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kCompdGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    // PolyhedralSurface lod2Solid patched to PolyhedralSurfaceZ:
    EXPECT_EQ(wkbPolyhedralSurfaceZ,
              layer->GetLayerDefn()->GetGeomFieldDefn(0)->GetType());
  }

  {
    CPLSetConfigOption("GML_REPORT_COMPD_CS", "TRUE");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GML_REPORT_COMPD_CS", nullptr);
    };
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kCompdGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(OGRGMLDataSourceTest, DealWithOgrSchemaOpenOption) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/schema_override.gml";
  constexpr char kGmlContent[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\"><gml:featureMember>"
      "<Item fid=\"item.1\"><f_str>hello</f_str><f_int>123</f_int>"
      "<gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point>"
      "</Item></gml:featureMember></root>";
  VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);

  // 1. Invalid JSON:
  {
    WithQuietHandler quiet_handler;
    const char* const open_opts[] = {"OGR_SCHEMA=invalid json", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    EXPECT_EQ(nullptr, ds);
  }

  // 2. Missing layer:
  {
    WithQuietHandler quiet_handler;
    const char* const open_opts[] = {
        "OGR_SCHEMA={\"layers\": [{\"name\": \"NonExistentLayer\", \"fields\": "
        "[]}]}",
        nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    EXPECT_EQ(nullptr, ds);
  }

  // 3. Partial schema override with named field rename and type change:
  {
    const char* const open_opts[] = {
        "WRITE_GFS=NO",
        "OGR_SCHEMA={\"layers\": [{\"name\": \"Item\", \"schemaType\": "
        "\"Partial\", "
        "\"fields\": [{\"name\": \"f_str\", \"newName\": \"f_renamed\", "
        "\"type\": \"String\", \"width\": 30}]}]}",
        nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_NE(-1, layer->GetLayerDefn()->GetFieldIndex("f_renamed"));
  }

  // 4. Full schema override removing non-listed fields, with wildcard "*":
  {
    const char* const open_opts[] = {
        "WRITE_GFS=NO",
        "OGR_SCHEMA={\"layers\": [{\"name\": \"*\", \"schemaType\": \"Full\", "
        "\"fields\": [{\"name\": \"f_int\", \"type\": \"Integer\"}]}]}",
        nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(-1, layer->GetLayerDefn()->GetFieldIndex("f_str"));
    EXPECT_NE(-1, layer->GetLayerDefn()->GetFieldIndex("f_int"));
  }

  // 5. Unnamed field override matching srcType:
  {
    const char* const open_opts[] = {
        "WRITE_GFS=NO",
        "OGR_SCHEMA={\"layers\": [{\"name\": \"Item\", "
        "\"fields\": [{\"srcType\": \"String\", \"type\": \"String\", "
        "\"width\": 42}]}]}",
        nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    int idx = layer->GetLayerDefn()->GetFieldIndex("f_str");
    ASSERT_NE(-1, idx);
    EXPECT_EQ(42, layer->GetLayerDefn()->GetFieldDefn(idx)->GetWidth());
  }

  // 6. Subtype override:
  {
    const char* const open_opts[] = {
        "WRITE_GFS=NO",
        "OGR_SCHEMA={\"layers\": [{\"name\": \"Item\", "
        "\"fields\": [{\"name\": \"f_int\", \"type\": \"Integer\", "
        "\"subType\": \"Int16\", \"precision\": 0}]}]}",
        nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    int idx = layer->GetLayerDefn()->GetFieldIndex("f_int");
    ASSERT_NE(-1, idx);
    EXPECT_EQ(OFSTInt16,
              layer->GetLayerDefn()->GetFieldDefn(idx)->GetSubType());
  }

  // 7. Named field not found in layer:
  {
    WithQuietHandler quiet_handler;
    const char* const open_opts[] = {
        "WRITE_GFS=NO",
        "OGR_SCHEMA={\"layers\": [{\"name\": \"Item\", "
        "\"fields\": [{\"name\": \"nonexistent_field\", \"type\": "
        "\"String\"}]}]}",
        nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    EXPECT_EQ(nullptr, ds);
  }
}

TEST_F(OGRGMLDataSourceTest, ExecuteSQLAndSingleFeatureLayer) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/sql_test.gml";
  constexpr char kXsdPath[] = "/vsimem/ogrgmlds_test/sql_test.xsd";
  constexpr char kGmlContent[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\">"
      "<gml:featureMember><Item fid=\"item.1\"><prop>10</prop>"
      "<gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point>"
      "</Item></gml:featureMember></root>";
  constexpr char kXsdContent[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "elementFormDefault=\"qualified\">"
      "<xs:import namespace=\"http://www.opengis.net/gml\"/>"
      "<xs:element name=\"Item\" type=\"ItemType\" "
      "substitutionGroup=\"gml:_Feature\"/>"
      "<xs:complexType name=\"ItemType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">"
      "<xs:sequence><xs:element name=\"geometryProperty\" "
      "type=\"gml:PointPropertyType\"/>"
      "<xs:element name=\"prop\" "
      "type=\"xs:integer\"/></xs:sequence></xs:extension></xs:complexContent></"
      "xs:complexType>"
      "</xs:schema>";
  VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);
  VsiMemTempWrapper f_xsd(kXsdPath, kXsdContent);

  const char* const open_opts[] = {"XSD=/vsimem/ogrgmlds_test/sql_test.xsd",
                                   nullptr};
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
  ASSERT_NE(nullptr, ds);

  // 1. SELECT ValidateSchema():
  OGRLayer* sqlLayer =
      ds->ExecuteSQL("SELECT ValidateSchema()", nullptr, nullptr);
  ASSERT_NE(nullptr, sqlLayer);
  EXPECT_STREQ("SELECT", sqlLayer->GetLayerDefn()->GetName());
  EXPECT_FALSE(sqlLayer->TestCapability(OLCRandomRead));

  sqlLayer->ResetReading();
  std::unique_ptr<OGRFeature> feat(sqlLayer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  // Second call returns nullptr:
  EXPECT_EQ(nullptr, sqlLayer->GetNextFeature());

  ds->ReleaseResultSet(sqlLayer);

  // 2. Generic SQL query delegation:
  OGRLayer* countLayer =
      ds->ExecuteSQL("SELECT COUNT(*) FROM Item", nullptr, nullptr);
  ASSERT_NE(nullptr, countLayer);
  std::unique_ptr<OGRFeature> countFeat(countLayer->GetNextFeature());
  ASSERT_NE(nullptr, countFeat);
  EXPECT_EQ(1, countFeat->GetFieldAsInteger(0));
  ds->ReleaseResultSet(countLayer);
}

TEST_F(OGRGMLDataSourceTest, CreateAndInsertHeaderAllFormatsAndOptions) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  // 1. GML3.2 output with INTERNAL schema (moves schema to top of file!),
  // all geometry types, precisions, unique constraints, comments, all field
  // types:
  {
    constexpr char kOutGml[] = "/vsimem/ogrgmlds_test/out_gml32.gml";
    VsiMemTempWrapper f_out(kOutGml, "");
    const char* const opts[] = {"FORMAT=GML3.2",
                                "XSISCHEMA=INTERNAL",
                                "SPACE_INDENTATION=YES",
                                "STRIP_PREFIX=NO",
                                "PREFIX=testapp",
                                "TARGET_NAMESPACE=http://example.com/testapp",
                                "GML_ID=my_gml_id",
                                "DESCRIPTION=Dataset Description",
                                "NAME=Dataset Name",
                                "BOUNDEDBY=YES",
                                "WRITE_FEATURE_BOUNDED_BY=YES",
                                "SRSNAME_FORMAT=OGC_URL",
                                "SRSDIMENSION_LOC=GEOMETRY,POSLIST",
                                nullptr};

    std::unique_ptr<GDALDataset> ds(driver->Create(
        kOutGml, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);

    // Layer 1 with various geometry types:
    OGRLayer* layer =
        ds->CreateLayer("LayerAllGeoms", &srs, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, layer);

    // Secondary geometry field with precision:
    OGRGeomFieldDefn geom2("geom2", wkbCurvePolygon);
    geom2.SetSpatialRef(&srs);
    OGRGeomCoordinatePrecision prec;
    prec.dfXYResolution = 0.001;
    prec.dfZResolution = 0.01;
    geom2.SetCoordinatePrecision(prec);
    EXPECT_EQ(OGRERR_NONE, layer->CreateGeomField(&geom2));

    // Field types:
    OGRFieldDefn f_int("f_int", OFTInteger);
    f_int.SetSubType(OFSTInt16);
    f_int.SetComment("Int16 comment");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_int));

    OGRFieldDefn f_bool("f_bool", OFTInteger);
    f_bool.SetSubType(OFSTBoolean);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));

    OGRFieldDefn f_int_list("f_int_list", OFTIntegerList);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_int_list));

    OGRFieldDefn f_i64("f_i64", OFTInteger64);
    f_i64.SetUnique(TRUE);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_i64));

    OGRFieldDefn f_i64_bool("f_i64_bool", OFTInteger64);
    f_i64_bool.SetSubType(OFSTBoolean);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_i64_bool));

    OGRFieldDefn f_i64_int16("f_i64_int16", OFTInteger64);
    f_i64_int16.SetSubType(OFSTInt16);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_i64_int16));

    OGRFieldDefn f_i64_list("f_i64_list", OFTInteger64List);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_i64_list));

    OGRFieldDefn f_real("f_real", OFTReal);
    f_real.SetWidth(10);
    f_real.SetPrecision(3);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_real));

    OGRFieldDefn f_float("f_float", OFTReal);
    f_float.SetSubType(OFSTFloat32);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_float));

    OGRFieldDefn f_real_list("f_real_list", OFTRealList);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_real_list));

    OGRFieldDefn f_str("f_str", OFTString);
    f_str.SetWidth(50);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_str));

    OGRFieldDefn f_str_list("f_str_list", OFTStringList);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_str_list));

    OGRFieldDefn f_date("f_date", OFTDate);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_date));

    OGRFieldDefn f_time("f_time", OFTTime);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_time));

    OGRFieldDefn f_datetime("f_datetime", OFTDateTime);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_datetime));

    // Write a feature:
    OGRFeature feat(layer->GetLayerDefn());
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(10, 20);
    ring.addPoint(10, 21);
    ring.addPoint(11, 21);
    ring.addPoint(11, 20);
    ring.addPoint(10, 20);
    poly.addRing(&ring);
    feat.SetGeometry(&poly);

    feat.SetField("f_int", 42);
    feat.SetField("f_bool", 1);
    const int ilist[] = {1, 2};
    feat.SetField("f_int_list", 2, ilist);
    feat.SetField("f_i64", 9876543210LL);
    feat.SetField("f_real", 123.456);
    feat.SetField("f_str", "test string");
    feat.SetField("f_date", 2026, 11, 3, 0, 0, 0, 0);
    feat.SetField("f_time", 0, 0, 0, 12, 34, 56, 0);
    feat.SetField("f_datetime", 2026, 11, 3, 12, 34, 56, 0);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));

    // Close dataset to flush and exercise InsertHeader() moving schema:
    ds->Close();
  }

  // 2. GML3Deegree format with GML_FEATURE_COLLECTION=YES, STRIP_PREFIX=YES:
  {
    constexpr char kOutDeegree[] = "/vsimem/ogrgmlds_test/out_deegree.gml";
    VsiMemTempWrapper f_deegree(kOutDeegree, "");
    const char* const opts[] = {
        "FORMAT=GML3Deegree", "GML_FEATURE_COLLECTION=YES", "STRIP_PREFIX=YES",
        "XSISCHEMA=EXTERNAL", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        kOutDeegree, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer =
        ds->CreateLayer("DeegreeLayer", nullptr, wkbLineString, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFeature feat(layer->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    feat.SetGeometry(&ls);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
    ds->Close();
  }

  // 3. GML2 format with XSISCHEMA=EXTERNAL and XSISCHEMAURI:
  {
    constexpr char kOutGml2[] = "/vsimem/ogrgmlds_test/out_gml2.gml";
    VsiMemTempWrapper f_gml2(kOutGml2, "");
    const char* const opts[] = {"FORMAT=GML2",
                                "XSISCHEMAURI=http://example.com/schema.xsd",
                                "GML3_LONGSRS=NO", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        kOutGml2, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("Gml2Layer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(1, 2);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
    ds->Close();
  }

  // 4. Non-seekable output via /vsigzip/:
  {
    constexpr char kUnderlyingGz[] = "/vsimem/ogrgmlds_test/out.gml.gz";
    constexpr char kOutGz[] = "/vsigzip//vsimem/ogrgmlds_test/out.gml.gz";
    VsiMemTempWrapper f_gz(kUnderlyingGz, "");
    const char* const opts[] = {"FORMAT=GML3", nullptr};
    std::unique_ptr<GDALDataset> ds(
        driver->Create(kOutGz, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("GzLayer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(10, 20);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
    ds->Close();
  }

  // 5. Non-seekable output via /vsizip/:
  {
    constexpr char kUnderlyingZip[] = "/vsimem/ogrgmlds_test/out.zip";
    constexpr char kOutZip[] = "/vsizip//vsimem/ogrgmlds_test/out.zip";
    VsiMemTempWrapper f_zip(kUnderlyingZip, "");
    const char* const opts[] = {"FORMAT=GML2", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        kOutZip, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("ZipLayer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(10, 20);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
    ds->Close();
  }
}

TEST_F(OGRGMLDataSourceTest, CloseBoundedByAndGlobalSRSVariants) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  // 1. GML3 with coordinate swap (EPSG:4326 lat/long) and 3D bounds:
  {
    constexpr char kPath[] = "/vsimem/ogrgmlds_test/bbox_swap.gml";
    VsiMemTempWrapper f_out(kPath, "");
    const char* const opts[] = {"FORMAT=GML3", "BOUNDEDBY=YES",
                                "SRSNAME_FORMAT=OGC_URN", nullptr};
    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRLayer* layer = ds->CreateLayer("SwapLayer", &srs, wkbPoint25D, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRGeomCoordinatePrecision prec;
    prec.dfXYResolution = 0.01;
    prec.dfZResolution = 0.1;
    layer->GetLayerDefn()->GetGeomFieldDefn(0)->SetCoordinatePrecision(prec);

    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(10, 20, 30);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
    ds->Close();
  }

  // 2. GML2 <gml:Box> with 3D <gml:Z>:
  {
    constexpr char kPath[] = "/vsimem/ogrgmlds_test/bbox_gml2_3d.gml";
    VsiMemTempWrapper f_out(kPath, "");
    const char* const opts[] = {"FORMAT=GML2", "BOUNDEDBY=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layer =
        ds->CreateLayer("Gml23DLayer", nullptr, wkbPoint25D, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(10, 20, 30);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
    ds->Close();
  }

  // 3. Empty dataset without layers:
  {
    constexpr char kPath[] = "/vsimem/ogrgmlds_test/empty_ds.gml";
    VsiMemTempWrapper f_out(kPath, "");
    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    ds->Close();
  }
}

TEST_F(OGRGMLDataSourceTest, DeclareNewWriteSRSTest) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  constexpr char kPath[] = "/vsimem/ogrgmlds_test/srs_decl.gml";
  VsiMemTempWrapper f_out(kPath, "");
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs1, srs2;
  srs1.importFromEPSG(4326);
  srs2.importFromEPSG(3857);

  // Layer 1 with EPSG:4326:
  OGRLayer* l1 = ds->CreateLayer("Layer1", &srs1, wkbPoint, nullptr);
  ASSERT_NE(nullptr, l1);

  // Layer 2 with differing SRS (EPSG:3857) -> invalidates global SRS:
  OGRLayer* l2 = ds->CreateLayer("Layer2", &srs2, wkbPoint, nullptr);
  ASSERT_NE(nullptr, l2);

  // Layer 3 with null SRS:
  OGRLayer* l3 = ds->CreateLayer("Layer3", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, l3);

  ds->Close();
}

TEST_F(OGRGMLDataSourceTest, TestCapabilityAndLayerManagementAndErrorHandling) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  constexpr char kPath[] = "/vsimem/ogrgmlds_test/caps.gml";
  VsiMemTempWrapper f_out(kPath, "");
  const char* const opts[] = {"FORMAT=GML3", nullptr};
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
  ASSERT_NE(nullptr, ds);

  // Capabilities:
  EXPECT_TRUE(ds->TestCapability(ODsCCreateLayer));
  EXPECT_TRUE(ds->TestCapability(ODsCCreateGeomFieldAfterCreateLayer));
  EXPECT_TRUE(ds->TestCapability(ODsCCurveGeometries));
  EXPECT_TRUE(ds->TestCapability(ODsCZGeometries));
  EXPECT_TRUE(ds->TestCapability(ODsCRandomLayerWrite));
  EXPECT_FALSE(ds->TestCapability("NonExistentCap"));

  // XML layer name cleaning:
  WithQuietHandler quiet_handler;
  OGRLayer* layerClean =
      ds->CreateLayer("123 Invalid Name!@#", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layerClean);
  EXPECT_STRNE("123 Invalid Name!@#", layerClean->GetName());

  // GetLayer boundary checks:
  EXPECT_EQ(nullptr, ds->GetLayer(-1));
  EXPECT_EQ(nullptr, ds->GetLayer(999));
  EXPECT_NE(nullptr, ds->GetLayer(0));

  ds->Close();

  // CreateLayer on read-only dataset fails:
  std::unique_ptr<GDALDataset> dsRead(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, dsRead);
  EXPECT_EQ(nullptr,
            dsRead->CreateLayer("FailLayer", nullptr, wkbPoint, nullptr));
}

TEST_F(OGRGMLDataSourceTest, WfsSchemaLocationAndTypeNamesPrefixRemovalAnd3D) {
  constexpr char kXsdPath[] = "/vsimem/ogrgmlds_test/wfs_prefix.xsd";
  constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/wfs_prefix.gml";
  constexpr char kXsdContent[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "elementFormDefault=\"qualified\">\n"
      "<xs:import namespace=\"http://www.opengis.net/gml\"/>\n"
      "<xs:element name=\"Item\" type=\"ItemType\" "
      "substitutionGroup=\"gml:_Feature\"/>\n"
      "<xs:complexType name=\"ItemType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">\n"
      "<xs:sequence>\n"
      "<xs:element name=\"geometryProperty\" type=\"gml:PointPropertyType\"/>\n"
      "<xs:element name=\"prop\" type=\"xs:string\"/>\n"
      "</xs:sequence></xs:extension></xs:complexContent></xs:complexType>\n"
      "<xs:element name=\"OtherItem\" type=\"OtherItemType\" "
      "substitutionGroup=\"gml:_Feature\"/>\n"
      "<xs:complexType name=\"OtherItemType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">\n"
      "<xs:sequence>\n"
      "<xs:element name=\"geometryProperty\" type=\"gml:PointPropertyType\"/>\n"
      "<xs:element name=\"prop\" type=\"xs:string\"/>\n"
      "</xs:sequence></xs:extension></xs:complexContent></xs:complexType>\n"
      "</xs:schema>";
  constexpr char kGmlContent[] =
      "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "xmlns:ns=\"http://example.com/ns\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
      "xsi:schemaLocation=\"http://example.com/ns "
      "http://example.com/"
      "wfs?service=WFS&amp;request=DescribeFeatureType&amp;typename=ns:Item; "
      "extra\">\n"
      "<gml:featureMember>\n"
      "<ns:Item fid=\"item.1\">\n"
      "<ns:prop>hello</ns:prop>\n"
      "<ns:geometryProperty><gml:Point srsDimension=\"3\"><gml:pos>1 2 "
      "3</gml:pos></gml:Point></ns:geometryProperty>\n"
      "</ns:Item>\n"
      "</gml:featureMember>\n"
      "</wfs:FeatureCollection>";
  VsiMemTempWrapper f_xsd(kXsdPath, kXsdContent);
  VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);

  CPLSetConfigOption("GML_DOWNLOAD_WFS_SCHEMA", "NO");
  absl::Cleanup reset_opt = [] {
    CPLSetConfigOption("GML_DOWNLOAD_WFS_SCHEMA", nullptr);
  };
  WithQuietHandler quiet_handler;
  const char* const open_opts[] = {"XSD=/vsimem/ogrgmlds_test/wfs_prefix.xsd",
                                   nullptr};
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_STREQ("Item", layer->GetName());
  EXPECT_EQ(wkbPoint25D, layer->GetLayerDefn()->GetGeomType());
}

TEST_F(OGRGMLDataSourceTest, RegistryBranchCoverage) {
  constexpr char kXsdPath[] = "/vsimem/ogrgmlds_test/reg_branch.xsd";
  constexpr char kGfsPath[] = "/vsimem/ogrgmlds_test/reg_branch.gfs";
  constexpr char kRegPath[] = "/vsimem/ogrgmlds_test/reg_branches.xml";

  constexpr char kXsdContent[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "elementFormDefault=\"qualified\">\n"
      "<xs:import namespace=\"http://www.opengis.net/gml\"/>\n"
      "<xs:element name=\"TypeB\" type=\"TypeBType\" "
      "substitutionGroup=\"gml:_Feature\"/>\n"
      "<xs:complexType name=\"TypeBType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">\n"
      "<xs:sequence>\n"
      "<xs:element name=\"geometryProperty\" type=\"gml:PointPropertyType\"/>\n"
      "<xs:element name=\"prop\" type=\"xs:string\"/>\n"
      "</xs:sequence></xs:extension></xs:complexContent></xs:complexType>\n"
      "</xs:schema>";

  constexpr char kGfsContent[] =
      "<GMLFeatureClassList>"
      "<GMLFeatureClass><Name>TypeC</Name><ElementPath>TypeC</ElementPath>"
      "<PropertyDefn><Name>prop</Name><ElementPath>prop</"
      "ElementPath><Type>String</Type></PropertyDefn>"
      "</GMLFeatureClass>"
      "<GMLFeatureClass><Name>TypeD</Name><ElementPath>TypeD</ElementPath>"
      "<PropertyDefn><Name>prop</Name><ElementPath>prop</"
      "ElementPath><Type>String</Type></PropertyDefn>"
      "</GMLFeatureClass>"
      "</GMLFeatureClassList>";

  constexpr char kRegContent[] =
      "<gml_registry>"
      "<namespace prefix=\"ns1\" uri=\"http://example.com/ns1\" "
      "useGlobalSRSName=\"true\">"
      "<featureType elementName=\"TypeA\" elementValue=\"ValA\" "
      "schemaLocation=\"/vsimem/ogrgmlds_test/nonexistent.xsd\"/>"
      "<featureType elementName=\"TypeB\" "
      "schemaLocation=\"/vsimem/ogrgmlds_test/reg_branch.xsd\"/>"
      "</namespace>"
      "<namespace prefix=\"\" uri=\"http://example.com/ns2\">"
      "<featureType elementName=\"TypeC\" elementValue=\"ValC\" "
      "gfsSchemaLocation=\"/vsimem/ogrgmlds_test/reg_branch.gfs\"/>"
      "<featureType elementName=\"TypeD\" "
      "gfsSchemaLocation=\"/vsimem/ogrgmlds_test/reg_branch.gfs\"/>"
      "</namespace>"
      "</gml_registry>";

  VsiMemTempWrapper f_xsd(kXsdPath, kXsdContent);
  VsiMemTempWrapper f_gfs(kGfsPath, kGfsContent);
  VsiMemTempWrapper f_reg(kRegPath, kRegContent);

  CPLSetConfigOption("GML_REGISTRY", kRegPath);
  absl::Cleanup reset_reg = [] { CPLSetConfigOption("GML_REGISTRY", nullptr); };

  // 1. Matches TypeA: prefix != "", elementValue != "", nonexistent xsd
  {
    constexpr char kGmlA[] = "/vsimem/ogrgmlds_test/reg_a.gml";
    constexpr char kContentA[] =
        "<root xmlns:gml=\"http://www.opengis.net/gml\" "
        "xmlns:ns1=\"http://example.com/ns1\">"
        "<gml:featureMember><ns1:TypeA>ValA</ns1:TypeA></gml:featureMember></"
        "root>";
    VsiMemTempWrapper f_gml(kGmlA, kContentA);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlA, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
  }

  // 2. Matches TypeB: prefix != "", elementValue == "", schemaLocation exists
  {
    constexpr char kGmlB[] = "/vsimem/ogrgmlds_test/reg_b.gml";
    constexpr char kContentB[] =
        "<root xmlns:gml=\"http://www.opengis.net/gml\" "
        "xmlns:ns1=\"http://example.com/ns1\">"
        "<gml:featureMember><ns1:TypeB "
        "fid=\"b.1\"><prop>val</prop></ns1:TypeB></gml:featureMember></root>";
    VsiMemTempWrapper f_gml(kGmlB, kContentB);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlB, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetLayerByName("TypeB"));
  }

  // 3. Matches TypeC: prefix == "", elementValue != "", gfsSchemaLocation
  // exists
  {
    constexpr char kGmlC[] = "/vsimem/ogrgmlds_test/reg_c.gml";
    constexpr char kContentC[] =
        "<root xmlns:gml=\"http://www.opengis.net/gml\" "
        "xmlns=\"http://example.com/ns2\">"
        "<gml:featureMember><TypeC>ValC</TypeC></gml:featureMember></root>";
    VsiMemTempWrapper f_gml(kGmlC, kContentC);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlC, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetLayerByName("TypeC"));
  }

  // 4. Matches TypeD: prefix == "", elementValue == "", gfsSchemaLocation
  // exists
  {
    constexpr char kGmlD[] = "/vsimem/ogrgmlds_test/reg_d.gml";
    constexpr char kContentD[] =
        "<root xmlns:gml=\"http://www.opengis.net/gml\" "
        "xmlns=\"http://example.com/ns2\">"
        "<gml:featureMember><TypeD "
        "fid=\"d.1\"><prop>val</prop></TypeD></gml:featureMember></root>";
    VsiMemTempWrapper f_gml(kGmlD, kContentD);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlD, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetLayerByName("TypeD"));
  }
}

TEST_F(OGRGMLDataSourceTest, WfsNumberReturnedAndVsimemOgrWfsAndGlobalSRS) {
  // Part 1: WFS 2.0 numberReturned and global SRS extent swap
  {
    constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/wfs_num_ret.gml";
    constexpr char kGmlContent[] =
        "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs/2.0\" "
        "xmlns:gml=\"http://www.opengis.net/gml/3.2\" "
        "numberReturned=\"1\">\n"
        "<gml:boundedBy><gml:Envelope srsName=\"urn:ogc:def:crs:EPSG::4326\">"
        "<gml:lowerCorner>10 20</gml:lowerCorner><gml:upperCorner>30 "
        "40</gml:upperCorner>"
        "</gml:Envelope></gml:boundedBy>\n"
        "<wfs:member>\n"
        "<Item gml:id=\"i.1\"><prop>val</prop>"
        "<geom><gml:Point><gml:pos>15 25</gml:pos></gml:Point></geom>"
        "</Item>\n"
        "</wfs:member>\n"
        "</wfs:FeatureCollection>";
    VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);

    CPLSetConfigOption("GML_INVERT_AXIS_ORDER_IF_LAT_LONG", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GML_INVERT_AXIS_ORDER_IF_LAT_LONG", nullptr);
    };

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, layer->GetFeatureCount(FALSE));
    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env));
    EXPECT_DOUBLE_EQ(25.0, env.MinX);
    EXPECT_DOUBLE_EQ(25.0, env.MaxX);
    EXPECT_DOUBLE_EQ(15.0, env.MinY);
    EXPECT_DOUBLE_EQ(15.0, env.MaxY);
  }

  // Part 2: /vsimem/_ogr_wfs_ filename setting bIsWFS and bExposeGMLId
  {
    constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/_ogr_wfs_sample.gml";
    constexpr char kGmlContent[] =
        "<G:FeatureCollection xmlns:G=\"http://example.com\" "
        "xmlns:gml=\"http://www.opengis.net/gml\">"
        "<gml:featureMember><Item "
        "gml:id=\"i.1\"><prop>1</prop></Item></gml:featureMember>"
        "</G:FeatureCollection>";
    VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_NE(-1, layer->GetLayerDefn()->GetFieldIndex("gml_id"));
  }
}

TEST_F(OGRGMLDataSourceTest, TranslateGMLSchemaSecondaryGeomSRS) {
  constexpr char kXsdPath[] = "/vsimem/ogrgmlds_test/multigeom.xsd";
  constexpr char kGmlPath[] = "/vsimem/ogrgmlds_test/multigeom.gml";
  constexpr char kXsdContent[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:gml=\"http://www.opengis.net/gml\" "
      "elementFormDefault=\"qualified\">\n"
      "<xs:import namespace=\"http://www.opengis.net/gml\"/>\n"
      "<xs:element name=\"Item\" type=\"ItemType\" "
      "substitutionGroup=\"gml:_Feature\"/>\n"
      "<xs:complexType name=\"ItemType\"><xs:complexContent><xs:extension "
      "base=\"gml:AbstractFeatureType\">\n"
      "<xs:sequence>\n"
      "<xs:element name=\"geom1\" type=\"gml:PointPropertyType\"/>\n"
      "<xs:element name=\"geom2\" type=\"gml:PointPropertyType\"/>\n"
      "</xs:sequence></xs:extension></xs:complexContent></xs:complexType>\n"
      "</xs:schema>";
  constexpr char kGmlContent[] =
      "<root xmlns:gml=\"http://www.opengis.net/gml\">\n"
      "<gml:featureMember><Item fid=\"i.1\">\n"
      "<geom1><gml:Point "
      "srsName=\"EPSG:4326\"><gml:coordinates>1,2</gml:coordinates></"
      "gml:Point></geom1>\n"
      "<geom2><gml:Point "
      "srsName=\"EPSG:3857\"><gml:coordinates>10,20</gml:coordinates></"
      "gml:Point></geom2>\n"
      "</Item></gml:featureMember></root>";
  VsiMemTempWrapper f_xsd(kXsdPath, kXsdContent);
  VsiMemTempWrapper f_gml(kGmlPath, kGmlContent);

  const char* const open_opts[] = {"XSD=/vsimem/ogrgmlds_test/multigeom.xsd",
                                   "FORCE_SRS_DETECTION=YES", nullptr};
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
  ASSERT_NE(nullptr, ds);
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  ASSERT_EQ(2, layer->GetLayerDefn()->GetGeomFieldCount());
  EXPECT_STREQ("geom1",
               layer->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("geom2",
               layer->GetLayerDefn()->GetGeomFieldDefn(1)->GetNameRef());
  const OGRSpatialReference* srs2 =
      layer->GetLayerDefn()->GetGeomFieldDefn(1)->GetSpatialRef();
  ASSERT_NE(nullptr, srs2);
  EXPECT_TRUE(srs2->IsProjected());
}

TEST_F(OGRGMLDataSourceTest, AdditionalCoverageBranches) {
  // 1. CheckHeader with CSW:
  EXPECT_TRUE(OGRGMLDataSource::CheckHeader("<csw:GetRecordsResponse>"));

  // 2. Create failure with invalid path:
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, driver->Create("/invalid_dir_98765/out.gml", 0, 0, 0,
                                      GDT_Unknown, nullptr));
  }

  // 3. /vsicurl_streaming/ and /vsicurl/ with SERVICE=WFS (bCheckAuxFile =
  // false):
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  "/vsicurl_streaming/http://invalid.example.com/test.gml",
                  GDAL_OF_VECTOR | GDAL_OF_READONLY));
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  "/vsicurl/http://invalid.example.com/test.gml?SERVICE=WFS",
                  GDAL_OF_VECTOR | GDAL_OF_READONLY));
  }
}

TEST_F(OGRGMLDataSourceTest, PrintXSDExtensionAllGeometryTypes) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  // 1. GML3 output with MultiPoint, MultiLineString, MultiCurve, MultiPolygon,
  // MultiSurface, GeometryCollection
  {
    constexpr char kOutGml[] = "/vsimem/ogrgmlds_test/xsd_all_geoms.gml";
    VsiMemTempWrapper f_out(kOutGml, "");
    const char* const opts[] = {"FORMAT=GML3", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        kOutGml, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr,
              ds->CreateLayer("L_MPoint", nullptr, wkbMultiPoint, nullptr));
    ASSERT_NE(nullptr,
              ds->CreateLayer("L_MLine", nullptr, wkbMultiLineString, nullptr));
    ASSERT_NE(nullptr,
              ds->CreateLayer("L_MCurve", nullptr, wkbMultiCurve, nullptr));
    ASSERT_NE(nullptr,
              ds->CreateLayer("L_MPoly", nullptr, wkbMultiPolygon, nullptr));
    ASSERT_NE(nullptr,
              ds->CreateLayer("L_MSurf", nullptr, wkbMultiSurface, nullptr));
    ASSERT_NE(nullptr, ds->CreateLayer("L_GeomColl", nullptr,
                                       wkbGeometryCollection, nullptr));
    ds->Close();
  }

  // 2. GML2 output with MultiLineString and MultiPolygon
  {
    constexpr char kOutGml2[] = "/vsimem/ogrgmlds_test/xsd_gml2_geoms.gml";
    VsiMemTempWrapper f_out(kOutGml2, "");
    const char* const opts[] = {"FORMAT=GML2", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        kOutGml2, 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->CreateLayer("L_MLine2", nullptr, wkbMultiLineString,
                                       nullptr));
    ASSERT_NE(nullptr,
              ds->CreateLayer("L_MPoly2", nullptr, wkbMultiPolygon, nullptr));
    ds->Close();
  }
}

// TODO: b/335317901 - Cover remaining WFS remote HTTP schema download
// (lines 1132-1152) and PrintLine write error branch (lines 3235-3238).

}  // namespace
}  // namespace autotest2
