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
// Tests GDALJP2Metadata and GMLJP2 box generation and parsing
// (gcore/gdaljp2metadata.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaljp2metadata.cpp

#include "third_party/gdal/gcore/gdaljp2metadata.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kVsiDir[] = "/vsimem/gdaljp2metadata_test";

constexpr std::array<GByte, 16> kMsiUuid = {0xb1, 0x4b, 0xf8, 0xbd, 0x08, 0x3d,
                                            0x4b, 0x43, 0xa5, 0xae, 0x8c, 0xd7,
                                            0xd5, 0xa6, 0xce, 0x03};
constexpr std::array<GByte, 16> kMsigUuid = {0x96, 0xA9, 0xF1, 0xF1, 0xDC, 0x98,
                                             0x40, 0x2D, 0xA7, 0xAE, 0xD6, 0x8E,
                                             0x34, 0x45, 0x18, 0x09};
constexpr std::array<GByte, 16> kXmpUuid = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9,
                                            0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94,
                                            0x91, 0xE3, 0xAF, 0xAC};
static_assert(kMsiUuid.size() == 16);
static_assert(kMsigUuid.size() == 16);
static_assert(kXmpUuid.size() == 16);

void WriteBoxToFile(VSILFILE* fp, const GDALJP2Box* box) {
  std::unique_ptr<GByte, CplFreeDeleter> data(box->GetWritableBoxData());
  ASSERT_NE(nullptr, data);
  const size_t len = static_cast<size_t>(box->GetDataLength() + 8);
  EXPECT_EQ(len, VSIFWriteL(data.get(), 1, len, fp));
}

void WriteTextFile(const char* filename, std::string_view content) {
  VSILFILE* fp = VSIFOpenL(filename, "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
}

TEST(GdalJp2MetadataTest, GeoTiffAndMsigAndBoxesRoundTrip) {
  GDALRegister_MEM();
  absl::Cleanup vsi_cleanup = [] { VSIRmdirRecursive(kVsiDir); };

  EXPECT_TRUE(GDALJP2Metadata::IsUUID_MSI(kMsiUuid.data()));
  EXPECT_FALSE(GDALJP2Metadata::IsUUID_MSI(kXmpUuid.data()));
  EXPECT_TRUE(GDALJP2Metadata::IsUUID_XMP(kXmpUuid.data()));
  EXPECT_FALSE(GDALJP2Metadata::IsUUID_XMP(kMsiUuid.data()));

  OGRSpatialReference srs_epsg;
  ASSERT_EQ(OGRERR_NONE, srs_epsg.importFromEPSG(4326));
  EXPECT_TRUE(GDALJP2Metadata::IsSRSCompatible(&srs_epsg));

  OGRSpatialReference srs_local;
  ASSERT_EQ(OGRERR_NONE, srs_local.SetLocalCS("MyLocalCS"));

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
  absl::Cleanup gcp_cleaner = [&gcp] { GDALDeinitGCPs(1, &gcp); };
  gcp.dfGCPPixel = 0.0;
  gcp.dfGCPLine = 0.0;
  gcp.dfGCPX = 10.0;
  gcp.dfGCPY = 20.0;
  md_epsg.SetGCPs(1, &gcp);
  md_epsg.SetGCPs(1, &gcp);  // Exercise replacing existing GCPs.

  CPLStringList rpc_md;
  rpc_md.AddString("LINE_OFF=0");
  rpc_md.AddString("SAMP_OFF=0");
  rpc_md.AddString("LAT_OFF=0");
  rpc_md.AddString("LONG_OFF=0");
  rpc_md.AddString("HEIGHT_OFF=0");
  rpc_md.AddString("LINE_SCALE=1");
  rpc_md.AddString("SAMP_SCALE=1");
  rpc_md.AddString("LAT_SCALE=1");
  rpc_md.AddString("LONG_SCALE=1");
  rpc_md.AddString("HEIGHT_SCALE=1");
  rpc_md.AddString("LINE_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
  rpc_md.AddString("LINE_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
  rpc_md.AddString("SAMP_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
  rpc_md.AddString("SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
  md_epsg.SetRPCMD(rpc_md.List());
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
      kMsigUuid.data(), static_cast<int>(msig_buf.size()), msig_buf.data()));
  ASSERT_NE(nullptr, box_msig);

  // Build jp2h with res -> resd (10 bytes).
  GDALJP2Box box_resd;
  box_resd.SetType("resd");
  constexpr std::array<GByte, 10> kResdData = {0x01, 0x00, 0x00, 0x01, 0x02,
                                               0x00, 0x00, 0x01, 0x02, 0x02};
  box_resd.SetWritableData(static_cast<int>(kResdData.size()),
                           kResdData.data());
  const GDALJP2Box* res_children[1] = {&box_resd};
  std::unique_ptr<GDALJP2Box> box_res(
      GDALJP2Box::CreateSuperBox("res ", 1, res_children));
  ASSERT_NE(nullptr, box_res);
  const GDALJP2Box* jp2h_children[1] = {box_res.get()};
  std::unique_ptr<GDALJP2Box> box_jp2h(
      GDALJP2Box::CreateSuperBox("jp2h", 1, jp2h_children));
  ASSERT_NE(nullptr, box_jp2h);

  // Build dataset with XMP, IPR, xml:BOX_0, and MultiDomainMetadata.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mem_ds(
      mem_drv->Create("mem", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  mem_ds->SetMetadataItem("FOO", "BAR");
  mem_ds->SetMetadataItem("CUSTOM_K", "CUSTOM_V", "CUSTOM_DOM");
  CPLStringList xmp_list;
  xmp_list.AddString("<x:xmpmeta/>");
  mem_ds->SetMetadata(xmp_list.List(), "xml:XMP");
  CPLStringList ipr_list;
  ipr_list.AddString("<ipr/>");
  mem_ds->SetMetadata(ipr_list.List(), "xml:IPR");
  CPLStringList box0_list;
  box0_list.AddString("<custom_box/>");
  mem_ds->SetMetadata(box0_list.List(), "xml:BOX_0");

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
  ASSERT_NE(nullptr, xml_boxes);
  absl::Cleanup xml_boxes_cleaner = [xml_boxes, &n_xml_boxes] {
    for (int i = 0; i < n_xml_boxes; ++i) {
      delete xml_boxes[i];
    }
    CPLFree(xml_boxes);
  };
  ASSERT_EQ(1, n_xml_boxes);

  // Also wrap a box inside a JUMBF ("jumb") superbox.
  constexpr std::array<GByte, 16> kJumbUuid = {1, 2,  3,  4,  5,  6,  7,  8,
                                               9, 10, 11, 12, 13, 14, 15, 16};
  std::unique_ptr<GDALJP2Box> jumb_desc(
      GDALJP2Box::CreateJUMBFDescriptionBox(kJumbUuid.data(), "test_jumb"));
  ASSERT_NE(nullptr, jumb_desc);
  const GDALJP2Box* jumb_payload[1] = {xml_boxes[0]};
  std::unique_ptr<GDALJP2Box> box_jumb(
      GDALJP2Box::CreateJUMBFBox(jumb_desc.get(), 1, jumb_payload));
  ASSERT_NE(nullptr, box_jumb);

  // Write all boxes to a /vsimem/ file (including duplicates to hit "Too many"
  // debug branches).
  constexpr char kJp2File[] = "/vsimem/gdaljp2metadata_test/test_boxes.jp2";
  {
    VSILFILE* fp = VSIFOpenL(kJp2File, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
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
  }

  // Read and parse with GeoJP2 priority.
  GDALJP2Metadata reader;
  int index_used = -1;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(
        reader.ReadAndParse("/vsimem/gdaljp2metadata_test/nonexistent.jp2"));
    CPLErrorReset();
  }
  EXPECT_TRUE(reader.ReadAndParse(kJp2File, 0, 1, 2, 3, &index_used));
  EXPECT_EQ(0, index_used);
  EXPECT_FALSE(reader.m_oSRS.IsEmpty());
  EXPECT_NE(nullptr, reader.pszXMPMetadata);
  EXPECT_NE(nullptr, reader.pszXMLIPR);
  EXPECT_NE(nullptr, reader.pszGDALMultiDomainMetadata);
  EXPECT_STREQ("3 (pixels/cm)", CSLFetchNameValue(reader.papszMetadata,
                                                  "TIFFTAG_RESOLUTIONUNIT"));

  // Read and parse with MSIG priority (GeoJP2 disabled).
  GDALJP2Metadata reader_msig;
  EXPECT_TRUE(reader_msig.ReadAndParse(kJp2File, -1, -1, 0, -1, &index_used));
  EXPECT_EQ(0, index_used);
  EXPECT_TRUE(reader_msig.m_bHaveGeoTransform);

  // World file fallback when GeoJP2, GMLJP2, MSIG are disabled.
  WriteTextFile("/vsimem/gdaljp2metadata_test/test_boxes.wld",
                "1.0\n0.0\n0.0\n-1.0\n100.5\n200.5\n");
  GDALJP2Metadata reader_wld;
  EXPECT_TRUE(reader_wld.ReadAndParse(kJp2File, -1, -1, -1, 0, &index_used));
  EXPECT_TRUE(reader_wld.m_bHaveGeoTransform);
}

TEST(GdalJp2MetadataTest, GmlJp2V1AndDictionaryAndAxisFlip) {
  absl::Cleanup vsi_cleanup = [] { VSIRmdirRecursive(kVsiDir); };

  // 1. CreateGMLJP2 with EPSG:4326 (triggers axis flip) and alt offset order.
  GDALJP2Metadata md_v1;
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  md_v1.SetSpatialRef(&srs);
  GDALGeoTransform gt(10.0, 0.0, -0.1, 50.0, 0.1, 0.0);
  md_v1.SetGeoTransform(gt);
  std::unique_ptr<GDALJP2Box> box_gml1;
  {
    CPLConfigOptionSetter alt_order("GDAL_JP2K_ALT_OFFSETVECTOR_ORDER", "TRUE",
                                    false);
    box_gml1.reset(md_v1.CreateGMLJP2(100, 100));
  }
  ASSERT_NE(nullptr, box_gml1);

  constexpr char kGml1File[] = "/vsimem/gdaljp2metadata_test/gml1.jp2";
  {
    VSILFILE* fp = VSIFOpenL(kGml1File, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    WriteBoxToFile(fp, box_gml1.get());
  }

  GDALJP2Metadata parsed_v1;
  EXPECT_TRUE(parsed_v1.ReadAndParse(kGml1File, -1, 0, -1, -1));
  EXPECT_TRUE(parsed_v1.m_bHaveGeoTransform);
  EXPECT_FALSE(parsed_v1.m_oSRS.IsEmpty());

  // 2. CreateGMLJP2 with custom non-EPSG SRS -> creates CRSDictionary.gml box.
  GDALJP2Metadata md_dict;
  OGRSpatialReference srs_no_auth;
  ASSERT_EQ(OGRERR_NONE,
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
                "UNIT[\"metre\",1]]"));
  md_dict.SetSpatialRef(&srs_no_auth);
  md_dict.SetGeoTransform(GDALGeoTransform(0.0, 10.0, 0.0, 0.0, 0.0, -10.0));
  std::unique_ptr<GDALJP2Box> box_dict(md_dict.CreateGMLJP2(50, 50));
  ASSERT_NE(nullptr, box_dict);

  constexpr char kDictFile[] = "/vsimem/gdaljp2metadata_test/gml_dict.jp2";
  {
    VSILFILE* fp = VSIFOpenL(kDictFile, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    WriteBoxToFile(fp, box_dict.get());
  }

  GDALJP2Metadata parsed_dict;
  EXPECT_TRUE(parsed_dict.ReadAndParse(kDictFile, -1, 0, -1, -1));
  EXPECT_TRUE(parsed_dict.m_bHaveGeoTransform);

  // 3. GMLJP2OVERRIDE config option (missing file and valid file with explicit
  //    Easting/Northing axisName and embedded NUL byte in CollectGMLData).
  {
    CPLConfigOptionSetter missing_ovr(
        "GMLJP2OVERRIDE", "/vsimem/gdaljp2metadata_test/no_such_override.xml",
        false);
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, md_v1.CreateGMLJP2(10, 10));
    CPLErrorReset();
  }

  constexpr char kOverrideFile[] = "/vsimem/gdaljp2metadata_test/override.xml";
  constexpr std::string_view kOverrideXml = R"xml(
<gml:FeatureCollection xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy>
    <gml:Envelope srsName="http://www.opengis.net/def/crs/EPSG/0/4326">
      <gml:lowerCorner>0 0</gml:lowerCorner>
      <gml:upperCorner>10 10</gml:upperCorner>
    </gml:Envelope>
  </gml:boundedBy>
  <gml:RectifiedGrid dimension="2">
    <gml:axisName>Easting</gml:axisName>
    <gml:axisName>Northing</gml:axisName>
    <gml:origin><gml:Point><gml:pos>10 20</gml:pos></gml:Point></gml:origin>
    <gml:offsetVector>1 0</gml:offsetVector>
    <gml:offsetVector>0 -1</gml:offsetVector>
  </gml:RectifiedGrid>
  <GDALMultiDomainMetadata><Metadata><MDI key="K">V</MDI></Metadata>
  </GDALMultiDomainMetadata>
</gml:FeatureCollection>)xml";
  WriteTextFile(kOverrideFile, kOverrideXml);
  std::unique_ptr<GDALJP2Box> box_override;
  {
    CPLConfigOptionSetter ovr_setter("GMLJP2OVERRIDE", kOverrideFile, false);
    box_override.reset(md_v1.CreateGMLJP2(10, 10));
  }
  ASSERT_NE(nullptr, box_override);

  // Also create an asoc/gml.data box whose xml payload has an embedded '\0'
  // before the closing tag, to exercise the NUL-to-newline fixup in
  // CollectGMLData (ticket #5760).
  std::string nul_xml = "<gml:FeatureCollection";
  nul_xml.push_back('\0');
  nul_xml += " xmlns:gml=\"http://www.opengis.net/gml\"/>";
  nul_xml.push_back('\0');
  std::unique_ptr<GDALJP2Box> nul_lbl(GDALJP2Box::CreateLblBox("nul.xml"));
  ASSERT_NE(nullptr, nul_lbl);
  GDALJP2Box nul_xml_box;
  nul_xml_box.SetType("xml ");
  nul_xml_box.SetWritableData(static_cast<int>(nul_xml.size()),
                              reinterpret_cast<const GByte*>(nul_xml.data()));
  const GDALJP2Box* nul_sub[2] = {nul_lbl.get(), &nul_xml_box};
  std::unique_ptr<GDALJP2Box> nul_asoc(GDALJP2Box::CreateAsocBox(2, nul_sub));
  ASSERT_NE(nullptr, nul_asoc);
  std::unique_ptr<GDALJP2Box> gml_data_lbl(
      GDALJP2Box::CreateLblBox("gml.data"));
  ASSERT_NE(nullptr, gml_data_lbl);
  const GDALJP2Box* top_sub[2] = {gml_data_lbl.get(), nul_asoc.get()};
  std::unique_ptr<GDALJP2Box> top_asoc(GDALJP2Box::CreateAsocBox(2, top_sub));
  ASSERT_NE(nullptr, top_asoc);

  constexpr char kOvrJp2[] = "/vsimem/gdaljp2metadata_test/ovr.jp2";
  {
    VSILFILE* fp = VSIFOpenL(kOvrJp2, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    WriteBoxToFile(fp, box_override.get());
    WriteBoxToFile(fp, top_asoc.get());
  }

  GDALJP2Metadata parsed_ovr;
  EXPECT_TRUE(parsed_ovr.ReadAndParse(kOvrJp2, -1, 0, -1, -1));
  EXPECT_NE(nullptr, parsed_ovr.pszGDALMultiDomainMetadata);
}

TEST(GdalJp2MetadataTest, CreateGMLJP2V2FullOptions) {
  GDALRegister_MEM();
  RegisterOGRGeoJSON();
  RegisterOGRGML();
  RegisterOGRKML();
  absl::Cleanup vsi_cleanup = [] { VSIRmdirRecursive(kVsiDir); };

  GDALJP2Metadata md;
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  md.SetSpatialRef(&srs);
  md.SetGeoTransform(GDALGeoTransform(10.0, 0.1, 0.0, 20.0, 0.0, -0.1));

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("mem", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  src_ds->SetMetadataItem("TEST_MD", "123");

  // 1. Default "YES"
  std::unique_ptr<GDALJP2Box> box_yes(
      md.CreateGMLJP2V2(10, 10, "YES", src_ds.get()));
  EXPECT_NE(nullptr, box_yes);

  // 2. Invalid JSON and missing definition file
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              md.CreateGMLJP2V2(10, 10,
                                "/vsimem/gdaljp2metadata_test/missing_def.json",
                                src_ds.get()));
    EXPECT_EQ(nullptr,
              md.CreateGMLJP2V2(10, 10, "{invalid json", src_ds.get()));
    CPLErrorReset();
  }

  // 3. Prepare auxiliary XML, GML, KML, SLD, XSD files in /vsimem/ for V2 JSON.
  WriteTextFile(
      "/vsimem/gdaljp2metadata_test/range_type.xml",
      R"(<swe:DataRecord xmlns:swe="http://www.opengis.net/swe/2.0"/>)");
  WriteTextFile("/vsimem/gdaljp2metadata_test/meta_iso.xml",
                "<gmljp2:isoMetadata/>");
  WriteTextFile("/vsimem/gdaljp2metadata_test/meta_eop.xml",
                "<eop:EarthObservation/>");
  WriteTextFile("/vsimem/gdaljp2metadata_test/style.sld",
                "<StyledLayerDescriptor/>");
  WriteTextFile("/vsimem/gdaljp2metadata_test/ext.xml", "<MyExtension/>");
  WriteTextFile(
      "/vsimem/gdaljp2metadata_test/annot.kml",
      R"(<kml xmlns="http://www.opengis.net/kml/2.2"><Document/></kml>)");
  WriteTextFile("/vsimem/gdaljp2metadata_test/schema.xsd",
                R"xml(<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="FeatureCollection" type="ogr:FeatureCollectionType" substitutionGroup="gml:AbstractGML"/>
</xs:schema>)xml");
  WriteTextFile("/vsimem/gdaljp2metadata_test/features.gml",
                R"gml(<ogr:FeatureCollection gml:id="FC1"
  xmlns:gml="http://www.opengis.net/gml/3.2"
  xmlns:ogr="http://example.com"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
  xsi:schemaLocation="http://example.com schema.xsd"/>)gml");
  WriteTextFile(
      "/vsimem/gdaljp2metadata_test/features.geojson",
      R"json({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"p1"},"geometry":{"type":"Point","coordinates":[1.0,2.0]}}]})json");

  constexpr std::string_view kJsonConfig = R"json({
    "root_instance": {
      "gml_id": "CUSTOM_ROOT_ID",
      "grid_coverage_range_type_field_predefined_name": "Color",
      "crs_url": false,
      "metadata": [
        "<gmljp2:dcMetadata/>",
        "/vsimem/gdaljp2metadata_test/meta_iso.xml",
        {
          "file": "/vsimem/gdaljp2metadata_test/meta_eop.xml",
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
        "/vsimem/gdaljp2metadata_test/annot.kml",
        "/vsimem/gdaljp2metadata_test/features.geojson"
      ],
      "gml_filelist": [
        "/vsimem/gdaljp2metadata_test/features.gml",
        {
          "file": "/vsimem/gdaljp2metadata_test/features.gml",
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
          "file": "/vsimem/gdaljp2metadata_test/features.geojson",
          "namespace_prefix": "myns",
          "namespace": "http://myns.org"
        }
      ],
      "styles": [
        "/vsimem/gdaljp2metadata_test/style.sld",
        {
          "file": "/vsimem/gdaljp2metadata_test/style.sld",
          "parent_node": "GridCoverage"
        }
      ],
      "extensions": [
        "/vsimem/gdaljp2metadata_test/ext.xml",
        {
          "file": "/vsimem/gdaljp2metadata_test/ext.xml",
          "parent_node": "GridCoverage"
        }
      ]
    },
    "boxes": [
      "/vsimem/gdaljp2metadata_test/schema.xsd",
      {
        "file": "/vsimem/gdaljp2metadata_test/schema.xsd",
        "label": "custom_schema.xsd"
      }
    ]
  })json";

  WriteTextFile("/vsimem/gdaljp2metadata_test/v2_config.json", kJsonConfig);
  {
    WithQuietHandler quiet;
    std::unique_ptr<GDALJP2Box> box_v2(md.CreateGMLJP2V2(
        10, 10, "/vsimem/gdaljp2metadata_test/v2_config.json", src_ds.get()));
    EXPECT_NE(nullptr, box_v2);
    CPLErrorReset();
  }

  // Also test Elevation_meter, Panchromatic, and grid_coverage_range_type_file.
  std::unique_ptr<GDALJP2Box> box_elev(md.CreateGMLJP2V2(
      10, 10,
      R"json({"root_instance":{"grid_coverage_range_type_field_predefined_name":"Elevation_meter"}})json",
      src_ds.get()));
  EXPECT_NE(nullptr, box_elev);

  std::unique_ptr<GDALJP2Box> box_pan(md.CreateGMLJP2V2(
      10, 10,
      R"json({"root_instance":{"grid_coverage_range_type_field_predefined_name":"Panchromatic"}})json",
      src_ds.get()));
  EXPECT_NE(nullptr, box_pan);

  std::unique_ptr<GDALJP2Box> box_rtfile(md.CreateGMLJP2V2(
      10, 10,
      R"json({"root_instance":{"grid_coverage_range_type_file":"/vsimem/gdaljp2metadata_test/range_type.xml"}})json",
      src_ds.get()));
  EXPECT_NE(nullptr, box_rtfile);

  WriteTextFile("/vsimem/gdaljp2metadata_test/grid_cov.xml",
                R"(<gmljp2:GMLJP2RectifiedGridCoverage gml:id="RGC_1"/>)");
  {
    WithQuietHandler quiet;
    std::unique_ptr<GDALJP2Box> box_gcfile(md.CreateGMLJP2V2(
        10, 10,
        R"json({"root_instance":{"grid_coverage_file":"/vsimem/gdaljp2metadata_test/grid_cov.xml","gml_filelist":[{"file":"/vsimem/gdaljp2metadata_test/features.gml","schema_location":"gmljp2://xml/missing_schema.xsd"}]}})json",
        src_ds.get()));
    EXPECT_NE(nullptr, box_gcfile);
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
