// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

#define private public
#include "third_party/gdal/gcore/gdaljp2metadata.h"
#undef private

namespace gdal {
namespace {

static const GByte kMsiUuid[16] = {0xb1, 0x4b, 0xf8, 0xbd, 0x08, 0x3d,
                                   0x4b, 0x43, 0xa5, 0xae, 0x8c, 0xd7,
                                   0xd5, 0xa6, 0xce, 0x03};
static const GByte kMsigUuid[16] = {0x96, 0xA9, 0xF1, 0xF1, 0xDC, 0x98,
                                    0x40, 0x2D, 0xA7, 0xAE, 0xD6, 0x8E,
                                    0x34, 0x45, 0x18, 0x09};
static const GByte kXmpUuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9,
                                   0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94,
                                   0x91, 0xE3, 0xAF, 0xAC};

void WriteBoxToFile(VSILFILE* fp, GDALJP2Box* box) {
  GByte* data = box->GetWritableBoxData();
  VSIFWriteL(data, 1, static_cast<size_t>(box->GetDataLength() + 8), fp);
  CPLFree(data);
}

void WriteTextFile(const char* filename, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(filename, "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL(content.data(), 1, content.size(), fp);
  VSIFCloseL(fp);
}

TEST(GdalJp2MetadataTest, GeoTiffAndMsigAndBoxesRoundTrip) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_TRUE(GDALJP2Metadata::IsUUID_MSI(kMsiUuid));
  EXPECT_FALSE(GDALJP2Metadata::IsUUID_MSI(kXmpUuid));
  EXPECT_TRUE(GDALJP2Metadata::IsUUID_XMP(kXmpUuid));
  EXPECT_FALSE(GDALJP2Metadata::IsUUID_XMP(kMsiUuid));

  OGRSpatialReference srs_epsg;
  srs_epsg.importFromEPSG(4326);
  EXPECT_TRUE(GDALJP2Metadata::IsSRSCompatible(&srs_epsg));

  OGRSpatialReference srs_local;
  srs_local.SetLocalCS("MyLocalCS");

  // Create two GeoTIFF boxes: first with LOCAL_CS, second with EPSG:4326.
  GDALJP2Metadata md_local;
  md_local.SetSpatialRef(&srs_local);
  GDALGeoTransform gt_local(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
  md_local.SetGeoTransform(gt_local);
  std::unique_ptr<GDALJP2Box> box_gtiff1(md_local.CreateJP2GeoTIFF());
  ASSERT_NE(nullptr, box_gtiff1);

  GDALJP2Metadata md_epsg;
  md_epsg.SetSpatialRef(&srs_epsg);
  md_epsg.bPixelIsPoint = true;
  GDALGeoTransform gt_epsg(10.0, 0.5, 0.0, 20.0, 0.0, -0.5);
  md_epsg.SetGeoTransform(gt_epsg);
  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  gcp.dfGCPPixel = 0.0;
  gcp.dfGCPLine = 0.0;
  gcp.dfGCPX = 10.0;
  gcp.dfGCPY = 20.0;
  md_epsg.SetGCPs(1, &gcp);
  md_epsg.SetGCPs(1, &gcp);  // Exercise replacing existing GCPs.
  GDALDeinitGCPs(1, &gcp);

  const char* const rpc_md[] = {
      "LINE_OFF=0",
      "SAMP_OFF=0",
      "LAT_OFF=0",
      "LONG_OFF=0",
      "HEIGHT_OFF=0",
      "LINE_SCALE=1",
      "SAMP_SCALE=1",
      "LAT_SCALE=1",
      "LONG_SCALE=1",
      "HEIGHT_SCALE=1",
      "LINE_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
      "LINE_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
      "SAMP_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
      "SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
      nullptr};
  md_epsg.SetRPCMD(const_cast<char**>(rpc_md));
  std::unique_ptr<GDALJP2Box> box_gtiff2(md_epsg.CreateJP2GeoTIFF());
  ASSERT_NE(nullptr, box_gtiff2);

  // Build an MSIG box (70 bytes starting with "MSIG/").
  std::vector<GByte> msig_buf(70, 0);
  std::memcpy(msig_buf.data(), "MSIG/", 5);
  double msig_vals[6] = {2.0, 0.0, 0.0, -2.0, 50.0, 60.0};
  for (int i = 0; i < 6; ++i) {
    CPL_LSBPTR64(&msig_vals[i]);
    std::memcpy(msig_buf.data() + 22 + 8 * i, &msig_vals[i], 8);
  }
  std::unique_ptr<GDALJP2Box> box_msig(GDALJP2Box::CreateUUIDBox(
      kMsigUuid, static_cast<int>(msig_buf.size()), msig_buf.data()));

  // Build jp2h with res -> resd (10 bytes).
  GDALJP2Box box_resd;
  box_resd.SetType("resd");
  const GByte resd_data[10] = {0x01, 0x00, 0x00, 0x01, 0x02,
                               0x00, 0x00, 0x01, 0x02, 0x02};
  box_resd.SetWritableData(10, resd_data);
  const GDALJP2Box* res_children[1] = {&box_resd};
  std::unique_ptr<GDALJP2Box> box_res(
      GDALJP2Box::CreateSuperBox("res ", 1, res_children));
  const GDALJP2Box* jp2h_children[1] = {box_res.get()};
  std::unique_ptr<GDALJP2Box> box_jp2h(
      GDALJP2Box::CreateSuperBox("jp2h", 1, jp2h_children));

  // Build dataset with XMP, IPR, xml:BOX_0, and MultiDomainMetadata.
  GDALDriverH hMemDrv = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, hMemDrv);
  std::unique_ptr<GDALDataset> mem_ds(GDALDataset::FromHandle(
      GDALCreate(hMemDrv, "mem", 4, 4, 1, GDT_Byte, nullptr)));
  mem_ds->SetMetadataItem("FOO", "BAR");
  mem_ds->SetMetadataItem("CUSTOM_K", "CUSTOM_V", "CUSTOM_DOM");
  const char* const xmp_list[] = {"<x:xmpmeta/>", nullptr};
  mem_ds->SetMetadata(const_cast<char**>(xmp_list), "xml:XMP");
  const char* const ipr_list[] = {"<ipr/>", nullptr};
  mem_ds->SetMetadata(const_cast<char**>(ipr_list), "xml:IPR");
  const char* const box0_list[] = {"<custom_box/>", nullptr};
  mem_ds->SetMetadata(const_cast<char**>(box0_list), "xml:BOX_0");

  std::unique_ptr<GDALJP2Box> box_mdmd(
      GDALJP2Metadata::CreateGDALMultiDomainMetadataXMLBox(mem_ds.get(),
                                                           FALSE));
  ASSERT_NE(nullptr, box_mdmd);
  std::unique_ptr<GDALJP2Box> box_xmp(
      GDALJP2Metadata::CreateXMPBox(mem_ds.get()));
  ASSERT_NE(nullptr, box_xmp);
  std::unique_ptr<GDALJP2Box> box_ipr(
      GDALJP2Metadata::CreateIPRBox(mem_ds.get()));
  ASSERT_NE(nullptr, box_ipr);
  int n_xml_boxes = 0;
  GDALJP2Box** xml_boxes =
      GDALJP2Metadata::CreateXMLBoxes(mem_ds.get(), &n_xml_boxes);
  ASSERT_EQ(1, n_xml_boxes);

  // Also wrap a box inside a JUMBF ("jumb") superbox.
  const GByte jumb_uuid[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                               9, 10, 11, 12, 13, 14, 15, 16};
  std::unique_ptr<GDALJP2Box> jumb_desc(
      GDALJP2Box::CreateJUMBFDescriptionBox(jumb_uuid, "test_jumb"));
  const GDALJP2Box* jumb_payload[1] = {xml_boxes[0]};
  std::unique_ptr<GDALJP2Box> box_jumb(
      GDALJP2Box::CreateJUMBFBox(jumb_desc.get(), 1, jumb_payload));

  // Write all boxes to a /vsimem/ file (including duplicates to hit "Too many"
  // debug branches).
  const char* jp2_file = "/vsimem/test_boxes.jp2";
  VSILFILE* fp = VSIFOpenL(jp2_file, "wb");
  ASSERT_NE(nullptr, fp);
  WriteBoxToFile(fp, box_gtiff1.get());
  WriteBoxToFile(fp, box_gtiff2.get());
  WriteBoxToFile(fp, box_gtiff2.get());  // 3rd GeoTIFF box -> ignored
  WriteBoxToFile(fp, box_msig.get());
  WriteBoxToFile(fp, box_msig.get());  // 2nd MSIG box -> ignored
  WriteBoxToFile(fp, box_xmp.get());
  WriteBoxToFile(fp, box_xmp.get());  // 2nd XMP box -> ignored
  WriteBoxToFile(fp, box_jp2h.get());
  WriteBoxToFile(fp, box_ipr.get());
  WriteBoxToFile(fp, box_ipr.get());  // 2nd IPR box -> ignored
  WriteBoxToFile(fp, box_mdmd.get());
  WriteBoxToFile(fp, box_mdmd.get());  // 2nd MDMD box -> ignored
  WriteBoxToFile(fp, box_jumb.get());
  VSIFCloseL(fp);

  delete xml_boxes[0];
  CPLFree(xml_boxes);

  // Read and parse with GeoJP2 priority.
  GDALJP2Metadata reader;
  int index_used = -1;
  EXPECT_FALSE(reader.ReadAndParse("/vsimem/nonexistent.jp2"));
  EXPECT_TRUE(reader.ReadAndParse(jp2_file, 0, 1, 2, 3, &index_used));
  EXPECT_EQ(0, index_used);
  EXPECT_FALSE(reader.m_oSRS.IsEmpty());
  EXPECT_NE(nullptr, reader.pszXMPMetadata);
  EXPECT_NE(nullptr, reader.pszXMLIPR);
  EXPECT_NE(nullptr, reader.pszGDALMultiDomainMetadata);
  EXPECT_STREQ("3 (pixels/cm)", CSLFetchNameValue(reader.papszMetadata,
                                                  "TIFFTAG_RESOLUTIONUNIT"));

  // Read and parse with MSIG priority (GeoJP2 disabled).
  GDALJP2Metadata reader_msig;
  EXPECT_TRUE(reader_msig.ReadAndParse(jp2_file, -1, -1, 0, -1, &index_used));
  EXPECT_EQ(0, index_used);
  EXPECT_TRUE(reader_msig.m_bHaveGeoTransform);

  // World file fallback when GeoJP2, GMLJP2, MSIG are disabled.
  WriteTextFile("/vsimem/test_boxes.wld",
                "1.0\n0.0\n0.0\n-1.0\n100.5\n200.5\n");
  GDALJP2Metadata reader_wld;
  EXPECT_TRUE(reader_wld.ReadAndParse(jp2_file, -1, -1, -1, 0, &index_used));
  EXPECT_TRUE(reader_wld.m_bHaveGeoTransform);

  VSIUnlink(jp2_file);
  VSIUnlink("/vsimem/test_boxes.wld");
  CPLPopErrorHandler();
}

TEST(GdalJp2MetadataTest, GmlJp2V1AndDictionaryAndAxisFlip) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. CreateGMLJP2 with EPSG:4326 (triggers axis flip) and alt offset order.
  CPLSetConfigOption("GDAL_JP2K_ALT_OFFSETVECTOR_ORDER", "TRUE");
  GDALJP2Metadata md_v1;
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  md_v1.SetSpatialRef(&srs);
  GDALGeoTransform gt(10.0, 0.0, -0.1, 50.0, 0.1, 0.0);
  md_v1.SetGeoTransform(gt);
  std::unique_ptr<GDALJP2Box> box_gml1(md_v1.CreateGMLJP2(100, 100));
  ASSERT_NE(nullptr, box_gml1);
  CPLSetConfigOption("GDAL_JP2K_ALT_OFFSETVECTOR_ORDER", nullptr);

  const char* gml1_file = "/vsimem/gml1.jp2";
  VSILFILE* fp = VSIFOpenL(gml1_file, "wb");
  WriteBoxToFile(fp, box_gml1.get());
  VSIFCloseL(fp);

  GDALJP2Metadata parsed_v1;
  EXPECT_TRUE(parsed_v1.ReadAndParse(gml1_file, -1, 0, -1, -1));
  EXPECT_TRUE(parsed_v1.m_bHaveGeoTransform);
  EXPECT_FALSE(parsed_v1.m_oSRS.IsEmpty());
  VSIUnlink(gml1_file);

  // 2. CreateGMLJP2 with custom non-EPSG SRS -> creates CRSDictionary.gml box!
  GDALJP2Metadata md_dict;
  OGRSpatialReference srs_no_auth;
  srs_no_auth.SetFromUserInput(
      "PROJCS[\"Custom_Mercator\","
      "GEOGCS[\"Custom_Geographic\","
      "DATUM[\"Custom_Datum\","
      "SPHEROID[\"Custom_Ellipsoid\",6378137,298.257223563]],"
      "PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Mercator_1SP\"],"
      "PARAMETER[\"central_meridian\",0],"
      "PARAMETER[\"scale_factor\",1],"
      "PARAMETER[\"false_easting\",0],"
      "PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1]]");
  md_dict.SetSpatialRef(&srs_no_auth);
  md_dict.SetGeoTransform(GDALGeoTransform(0.0, 10.0, 0.0, 0.0, 0.0, -10.0));
  std::unique_ptr<GDALJP2Box> box_dict(md_dict.CreateGMLJP2(50, 50));
  ASSERT_NE(nullptr, box_dict);

  const char* dict_file = "/vsimem/gml_dict.jp2";
  fp = VSIFOpenL(dict_file, "wb");
  WriteBoxToFile(fp, box_dict.get());
  VSIFCloseL(fp);

  GDALJP2Metadata parsed_dict;
  EXPECT_TRUE(parsed_dict.ReadAndParse(dict_file, -1, 0, -1, -1));
  EXPECT_TRUE(parsed_dict.m_bHaveGeoTransform);
  // Also directly test GMLSRSLookup with an explicit gml:id="ogrcrs1" entry
  // across all supported URN prefixes (urn:jp2k:xml:, urn:ogc:tc:gmljp2:xml:,
  // gmljp2://xml/):
  const char* const kValidDictXML =
      "<gml:Dictionary gml:id=\"D1\" xmlns:gml=\"http://www.opengis.net/gml\">"
      "  <gml:dictionaryEntry>"
      "    <gml:GeographicCRS gml:id=\"ogrcrs1\">"
      "      <gml:srsName>WGS 84</gml:srsName>"
      "      <gml:srsID><gml:name "
      "codeSpace=\"urn:ogc:def:crs:EPSG::\">4326</gml:name></gml:srsID>"
      "      <gml:usesEllipsoidalCS>"
      "        <gml:EllipsoidalCS gml:id=\"cs1\">"
      "          <gml:csName>ellipsoidal</gml:csName>"
      "          <gml:usesAxis>"
      "            <gml:CoordinateSystemAxis gml:id=\"a1\" "
      "gml:uom=\"urn:ogc:def:uom:EPSG::9122\">"
      "              <gml:name>Latitude</gml:name>"
      "              <gml:axisAbbrev>lat</gml:axisAbbrev>"
      "              <gml:axisDirection>north</gml:axisDirection>"
      "            </gml:CoordinateSystemAxis>"
      "          </gml:usesAxis>"
      "          <gml:usesAxis>"
      "            <gml:CoordinateSystemAxis gml:id=\"a2\" "
      "gml:uom=\"urn:ogc:def:uom:EPSG::9122\">"
      "              <gml:name>Longitude</gml:name>"
      "              <gml:axisAbbrev>lon</gml:axisAbbrev>"
      "              <gml:axisDirection>east</gml:axisDirection>"
      "            </gml:CoordinateSystemAxis>"
      "          </gml:usesAxis>"
      "        </gml:EllipsoidalCS>"
      "      </gml:usesEllipsoidalCS>"
      "      <gml:usesGeodeticDatum>"
      "        <gml:GeodeticDatum gml:id=\"d1\">"
      "          <gml:datumName>WGS_1984</gml:datumName>"
      "          <gml:usesPrimeMeridian>"
      "            <gml:PrimeMeridian gml:id=\"pm1\">"
      "              <gml:meridianName>Greenwich</gml:meridianName>"
      "              <gml:greenwichLongitude><gml:angle "
      "uom=\"urn:ogc:def:uom:EPSG::9102\">0</gml:angle></"
      "gml:greenwichLongitude>"
      "            </gml:PrimeMeridian>"
      "          </gml:usesPrimeMeridian>"
      "          <gml:usesEllipsoid>"
      "            <gml:Ellipsoid gml:id=\"e1\">"
      "              <gml:ellipsoidName>WGS 84</gml:ellipsoidName>"
      "              <gml:semiMajorAxis "
      "uom=\"urn:ogc:def:uom:EPSG::9001\">6378137</gml:semiMajorAxis>"
      "              <gml:secondDefiningParameter><gml:inverseFlattening "
      "uom=\"urn:ogc:def:uom:EPSG::9201\">298.257223563</"
      "gml:inverseFlattening></gml:secondDefiningParameter>"
      "            </gml:Ellipsoid>"
      "          </gml:usesEllipsoid>"
      "        </gml:GeodeticDatum>"
      "      </gml:usesGeodeticDatum>"
      "    </gml:GeographicCRS>"
      "  </gml:dictionaryEntry>"
      "</gml:Dictionary>";
  parsed_dict.papszGMLMetadata = CSLSetNameValue(
      parsed_dict.papszGMLMetadata, "CRSDictionary.gml", kValidDictXML);
  EXPECT_TRUE(
      parsed_dict.GMLSRSLookup("gmljp2://xml/CRSDictionary.gml#ogrcrs1"));
  EXPECT_TRUE(
      parsed_dict.GMLSRSLookup("urn:jp2k:xml:CRSDictionary.gml#ogrcrs1"));
  EXPECT_TRUE(parsed_dict.GMLSRSLookup(
      "urn:ogc:tc:gmljp2:xml:CRSDictionary.gml#ogrcrs1"));
  EXPECT_FALSE(parsed_dict.GMLSRSLookup("urn:ogc:def:crs:NO_COLON_OR_HASH"));
  EXPECT_FALSE(parsed_dict.GMLSRSLookup("gmljp2://xml/Missing.gml#ogrcrs1"));
  EXPECT_FALSE(
      parsed_dict.GMLSRSLookup("gmljp2://xml/CRSDictionary.gml#missing_id"));
  VSIUnlink(dict_file);

  // 3. GMLJP2OVERRIDE config option (missing file and valid file with explicit
  // Easting/Northing axisName and embedded NUL byte in CollectGMLData).
  CPLSetConfigOption("GMLJP2OVERRIDE", "/vsimem/no_such_override.xml");
  EXPECT_EQ(nullptr, md_v1.CreateGMLJP2(10, 10));

  const char* override_file = "/vsimem/override.xml";
  std::string kOverrideXML =
      "<gml:FeatureCollection xmlns:gml=\"http://www.opengis.net/gml\">"
      "  <gml:boundedBy>"
      "    <gml:Envelope "
      "srsName=\"http://www.opengis.net/def/crs/EPSG/0/4326\">"
      "      <gml:lowerCorner>0 0</gml:lowerCorner>"
      "      <gml:upperCorner>10 10</gml:upperCorner>"
      "    </gml:Envelope>"
      "  </gml:boundedBy>"
      "  <gml:RectifiedGrid dimension=\"2\">"
      "    <gml:axisName>Easting</gml:axisName>"
      "    <gml:axisName>Northing</gml:axisName>"
      "    <gml:origin><gml:Point><gml:pos>10 "
      "20</gml:pos></gml:Point></gml:origin>"
      "    <gml:offsetVector>1 0</gml:offsetVector>"
      "    <gml:offsetVector>0 -1</gml:offsetVector>"
      "  </gml:RectifiedGrid>"
      "  <GDALMultiDomainMetadata><Metadata><MDI key=\"K\">V</MDI></Metadata>"
      "  </GDALMultiDomainMetadata>"
      "</gml:FeatureCollection>";
  WriteTextFile(override_file, kOverrideXML);
  CPLSetConfigOption("GMLJP2OVERRIDE", override_file);
  std::unique_ptr<GDALJP2Box> box_override(md_v1.CreateGMLJP2(10, 10));
  ASSERT_NE(nullptr, box_override);
  CPLSetConfigOption("GMLJP2OVERRIDE", nullptr);

  // Also create an asoc/gml.data box whose xml payload has an embedded '\0'
  // before the closing tag, to exercise the NUL-to-newline fixup in
  // CollectGMLData (ticket #5760).
  std::string nul_xml = "<gml:FeatureCollection";
  nul_xml.push_back('\0');
  nul_xml += " xmlns:gml=\"http://www.opengis.net/gml\"/>";
  nul_xml.push_back('\0');
  std::unique_ptr<GDALJP2Box> nul_lbl(GDALJP2Box::CreateLblBox("nul.xml"));
  GDALJP2Box nul_xml_box;
  nul_xml_box.SetType("xml ");
  nul_xml_box.SetWritableData(static_cast<int>(nul_xml.size()),
                              reinterpret_cast<const GByte*>(nul_xml.data()));
  const GDALJP2Box* nul_sub[2] = {nul_lbl.get(), &nul_xml_box};
  std::unique_ptr<GDALJP2Box> nul_asoc(GDALJP2Box::CreateAsocBox(2, nul_sub));
  std::unique_ptr<GDALJP2Box> gml_data_lbl(
      GDALJP2Box::CreateLblBox("gml.data"));
  const GDALJP2Box* top_sub[2] = {gml_data_lbl.get(), nul_asoc.get()};
  std::unique_ptr<GDALJP2Box> top_asoc(GDALJP2Box::CreateAsocBox(2, top_sub));

  const char* ovr_jp2 = "/vsimem/ovr.jp2";
  fp = VSIFOpenL(ovr_jp2, "wb");
  WriteBoxToFile(fp, box_override.get());
  WriteBoxToFile(fp, top_asoc.get());
  VSIFCloseL(fp);

  GDALJP2Metadata parsed_ovr;
  EXPECT_TRUE(parsed_ovr.ReadAndParse(ovr_jp2, -1, 0, -1, -1));
  EXPECT_NE(nullptr, parsed_ovr.pszGDALMultiDomainMetadata);

  VSIUnlink(ovr_jp2);
  VSIUnlink(override_file);
  CPLPopErrorHandler();
}

TEST(GdalJp2MetadataTest, CreateGMLJP2V2FullOptions) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  GDALJP2Metadata md;
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  md.SetSpatialRef(&srs);
  md.SetGeoTransform(GDALGeoTransform(10.0, 0.1, 0.0, 20.0, 0.0, -0.1));

  GDALDriverH hMemDrv = GDALGetDriverByName("MEM");
  std::unique_ptr<GDALDataset> src_ds(GDALDataset::FromHandle(
      GDALCreate(hMemDrv, "mem", 10, 10, 1, GDT_Byte, nullptr)));
  src_ds->SetMetadataItem("TEST_MD", "123");

  // 1. Default "YES"
  std::unique_ptr<GDALJP2Box> box_yes(
      md.CreateGMLJP2V2(10, 10, "YES", src_ds.get()));
  EXPECT_NE(nullptr, box_yes);

  // 2. Invalid JSON and missing definition file
  EXPECT_EQ(nullptr, md.CreateGMLJP2V2(10, 10, "/vsimem/missing_def.json",
                                       src_ds.get()));
  EXPECT_EQ(nullptr, md.CreateGMLJP2V2(10, 10, "{invalid json", src_ds.get()));

  // 3. Prepare auxiliary XML, GML, KML, SLD, XSD files in /vsimem/ for V2 JSON.
  WriteTextFile(
      "/vsimem/range_type.xml",
      "<swe:DataRecord xmlns:swe=\"http://www.opengis.net/swe/2.0\"/>");
  WriteTextFile("/vsimem/meta_iso.xml", "<gmljp2:isoMetadata/>");
  WriteTextFile("/vsimem/meta_eop.xml", "<eop:EarthObservation/>");
  WriteTextFile("/vsimem/style.sld", "<StyledLayerDescriptor/>");
  WriteTextFile("/vsimem/ext.xml", "<MyExtension/>");
  WriteTextFile(
      "/vsimem/annot.kml",
      "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document/></kml>");
  WriteTextFile("/vsimem/schema.xsd",
                "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\">"
                "  <xs:element name=\"FeatureCollection\" "
                "type=\"ogr:FeatureCollectionType\" "
                "substitutionGroup=\"gml:AbstractGML\"/>"
                "</xs:schema>");
  WriteTextFile("/vsimem/features.gml",
                "<ogr:FeatureCollection gml:id=\"FC1\" "
                "xmlns:gml=\"http://www.opengis.net/gml/3.2\" "
                "xmlns:ogr=\"http://example.com\" "
                "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                "xsi:schemaLocation=\"http://example.com schema.xsd\"/>");
  WriteTextFile("/vsimem/features.geojson",
                "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":"
                "\"Feature\",\"properties\":{\"name\":\"p1\"},\"geometry\":{"
                "\"type\":\"Point\",\"coordinates\":[1.0,2.0]}}]}");

  const std::string kJsonConfig = R"json({
    "root_instance": {
      "gml_id": "CUSTOM_ROOT_ID",
      "grid_coverage_range_type_field_predefined_name": "Color",
      "crs_url": false,
      "metadata": [
        "<gmljp2:dcMetadata/>",
        "/vsimem/meta_iso.xml",
        {
          "file": "/vsimem/meta_eop.xml",
          "parent_node": "GridCoverage"
        },
        {
          "content": "<gmljp2:metadata><sub/></gmljp2:metadata>",
          "parent_node": "CoverageCollection"
        },
        {
          "gdal_metadata": true,
          "parent_node": "GridCoverage"
        },
        {
          "content": "<invalidRoot/>",
          "parent_node": "InvalidParent"
        }
      ],
      "annotations": [
        "/vsimem/annot.kml",
        "/vsimem/features.geojson"
      ],
      "gml_filelist": [
        "/vsimem/features.gml",
        {
          "file": "/vsimem/features.gml",
          "inline": false,
          "parent_node": "GridCoverage",
          "namespace": "http://example.com",
          "schema_location": "gmljp2://xml/schema.xsd"
        },
        {
          "remote_resource": "http://example.com/remote.gml",
          "parent_node": "CoverageCollection"
        },
        {
          "file": "/vsimem/features.geojson",
          "namespace_prefix": "myns",
          "namespace": "http://myns.org"
        }
      ],
      "styles": [
        "/vsimem/style.sld",
        {
          "file": "/vsimem/style.sld",
          "parent_node": "GridCoverage"
        }
      ],
      "extensions": [
        "/vsimem/ext.xml",
        {
          "file": "/vsimem/ext.xml",
          "parent_node": "GridCoverage"
        }
      ]
    },
    "boxes": [
      "/vsimem/schema.xsd",
      {
        "file": "/vsimem/schema.xsd",
        "label": "custom_schema.xsd"
      }
    ]
  })json";

  WriteTextFile("/vsimem/v2_config.json", kJsonConfig);
  std::unique_ptr<GDALJP2Box> box_v2(
      md.CreateGMLJP2V2(10, 10, "/vsimem/v2_config.json", src_ds.get()));
  EXPECT_NE(nullptr, box_v2);

  // Also test Elevation_meter, Panchromatic, and grid_coverage_range_type_file.
  std::unique_ptr<GDALJP2Box> box_elev(md.CreateGMLJP2V2(
      10, 10,
      R"({"root_instance":{"grid_coverage_range_type_field_predefined_name":"Elevation_meter"}})",
      src_ds.get()));
  EXPECT_NE(nullptr, box_elev);

  std::unique_ptr<GDALJP2Box> box_pan(md.CreateGMLJP2V2(
      10, 10,
      R"({"root_instance":{"grid_coverage_range_type_field_predefined_name":"Panchromatic"}})",
      src_ds.get()));
  EXPECT_NE(nullptr, box_pan);

  std::unique_ptr<GDALJP2Box> box_rtfile(md.CreateGMLJP2V2(
      10, 10,
      R"({"root_instance":{"grid_coverage_range_type_file":"/vsimem/range_type.xml"}})",
      src_ds.get()));
  EXPECT_NE(nullptr, box_rtfile);

  WriteTextFile("/vsimem/grid_cov.xml",
                "<gmljp2:GMLJP2RectifiedGridCoverage gml:id=\"RGC_1\"/>");
  std::unique_ptr<GDALJP2Box> box_gcfile(md.CreateGMLJP2V2(
      10, 10,
      R"({"root_instance":{"grid_coverage_file":"/vsimem/grid_cov.xml","gml_filelist":[{"file":"/vsimem/features.gml","schema_location":"gmljp2://xml/missing_schema.xsd"}]}})",
      src_ds.get()));
  EXPECT_NE(nullptr, box_gcfile);
  VSIUnlink("/vsimem/grid_cov.xml");

  VSIUnlink("/vsimem/range_type.xml");
  VSIUnlink("/vsimem/meta_iso.xml");
  VSIUnlink("/vsimem/meta_eop.xml");
  VSIUnlink("/vsimem/style.sld");
  VSIUnlink("/vsimem/ext.xml");
  VSIUnlink("/vsimem/annot.kml");
  VSIUnlink("/vsimem/schema.xsd");
  VSIUnlink("/vsimem/features.gml");
  VSIUnlink("/vsimem/features.geojson");
  VSIUnlink("/vsimem/v2_config.json");
  CPLPopErrorHandler();
}

}  // namespace
}  // namespace gdal
