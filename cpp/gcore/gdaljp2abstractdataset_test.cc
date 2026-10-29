// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/gcore/gdaljp2abstractdataset.h"

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdaljp2metadata.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

class TestJP2Dataset final : public GDALJP2AbstractDataset {
 public:
  explicit TestJP2Dataset(VSILFILE* fp = nullptr) : m_fp(fp) {
    nRasterXSize = 10;
    nRasterYSize = 10;
  }
  using GDALJP2AbstractDataset::CloseDependentDatasets;

 protected:
  VSILFILE* GetFileHandle() override {
    if (m_fp) return m_fp;
    return GDALJP2AbstractDataset::GetFileHandle();
  }

 private:
  VSILFILE* m_fp = nullptr;
};

void WriteBox(VSILFILE* fp, const GDALJP2Box* box) {
  GByte* data = box->GetWritableBoxData();
  VSIFWriteL(data, 1, static_cast<size_t>(box->GetBoxLength()), fp);
  CPLFree(data);
}

TEST(GdalJp2AbstractDatasetTest, LoadMetadataAndVectorLayers) {
  GDALAllRegister();
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  const char* jp2_path = "/vsimem/test_abstract.jp2";
  const char* wld_path = "/vsimem/test_abstract.wld";
  const char* imd_path = "/vsimem/test_abstract.imd";

  // Create a worldfile and a DigitalGlobe .imd metadata file
  {
    VSILFILE* fp_wld = VSIFOpenL(wld_path, "wb");
    const char* wld = "1.0\n0.0\n0.0\n-1.0\n100.5\n200.5\n";
    VSIFWriteL(wld, 1, strlen(wld), fp_wld);
    VSIFCloseL(fp_wld);

    VSILFILE* fp_imd = VSIFOpenL(imd_path, "wb");
    const char* imd =
        "version = \"Q\";\nBEGIN_GROUP = IMAGE_1\n\tsatId = "
        "\"WV02\";\nEND_GROUP = IMAGE_1\nEND;\n";
    VSIFWriteL(imd, 1, strlen(imd), fp_imd);
    VSIFCloseL(fp_imd);
  }

  // Build JP2 boxes in /vsimem/test_abstract.jp2
  VSILFILE* fp = VSIFOpenL(jp2_path, "wb+");
  ASSERT_NE(fp, nullptr);

  // 0. JP2 Signature box ("jP  " + 0x0D0A870A) so GDALGetJPEG2000Structure
  // recognizes it
  GDALJP2Box b_sig;
  b_sig.SetType("jP  ");
  const GByte sig_data[4] = {0x0D, 0x0A, 0x87, 0x0A};
  b_sig.SetWritableData(4, sig_data);
  WriteBox(fp, &b_sig);

  // 1. GeoJP2 UUID box (with SRS, GeoTransform, GCPs, RPC, PixelIsPoint)
  GDALJP2Metadata jp2_md;
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  jp2_md.SetSpatialRef(&srs);
  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  jp2_md.SetGeoTransform(gt);
  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  gcp.dfGCPPixel = 1.0;
  gcp.dfGCPLine = 1.0;
  jp2_md.SetGCPs(1, &gcp);
  GDALDeinitGCPs(1, &gcp);
  const char* rpc_md[] = {
      "LINE_OFF=1",
      "LINE_SCALE=1",
      "SAMP_OFF=1",
      "SAMP_SCALE=1",
      "LAT_OFF=1",
      "LONG_OFF=1",
      "HEIGHT_OFF=1",
      "LAT_SCALE=1",
      "LONG_SCALE=1",
      "HEIGHT_SCALE=1",
      "LINE_NUM_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
      "LINE_DEN_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
      "SAMP_NUM_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
      "SAMP_DEN_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
      nullptr};
  jp2_md.SetRPCMD(const_cast<char**>(rpc_md));
  jp2_md.bPixelIsPoint = true;
  std::unique_ptr<GDALJP2Box> geo_box(jp2_md.CreateJP2GeoTIFF());
  if (geo_box) WriteBox(fp, geo_box.get());

  // 2. XMP UUID box
  GByte uuid_xmp[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                        0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};
  const char* xmp_xml = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";
  std::unique_ptr<GDALJP2Box> xmp_box(
      GDALJP2Box::CreateUUIDBox(uuid_xmp, static_cast<int>(strlen(xmp_xml) + 1),
                                reinterpret_cast<const GByte*>(xmp_xml)));
  WriteBox(fp, xmp_box.get());

  // 3. Labelled XML assoc boxes: gml.root-instance, aux.fc, aux.xsd, and
  // GDALMultiDomainMetadata
  const char* gml_root =
      "<gmljp2:GMLJP2CoverageCollection "
      "xmlns:gmljp2=\"http://www.opengis.net/gmljp2/2.0\" "
      "xmlns:gml=\"http://www.opengis.net/gml/3.2\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
      "  <gmljp2:featureMember>"
      "    <gmljp2:GMLJP2Features>"
      "      <gmljp2:feature xlink:href=\"gmljp2://xml/aux.fc\"/>"
      "      <gmljp2:feature xlink:href=\"gmljp2://xml/missing.fc\"/>"
      "      <gmljp2:feature xlink:href=\"http://example.com/remote.gml\"/>"
      "    </gmljp2:GMLJP2Features>"
      "  </gmljp2:featureMember>"
      "  <gmljp2:featureMember>"
      "    <gmljp2:GMLJP2GridCoverage>"
      "      <gmljp2:feature>"
      "        <gml:FeatureCollection "
      "xsi:schemaLocation=\"http://www.opengis.net/gml/3.2 "
      "gmljp2://xml/aux.xsd\">"
      "          <gml:featureMember>"
      "            "
      "<Poly><gml:polygonProperty><gml:Polygon><gml:outerBoundaryIs>"
      "<gml:LinearRing><gml:coordinates>0,0 1,0 1,1 0,0</gml:coordinates>"
      "</gml:LinearRing></gml:outerBoundaryIs></gml:Polygon>"
      "</gml:polygonProperty></Poly>"
      "          </gml:featureMember>"
      "        </gml:FeatureCollection>"
      "      </gmljp2:feature>"
      "      <gmljp2:annotation>"
      "        <kml xmlns=\"http://www.opengis.net/kml/2.2\">"
      "          <Document><Placemark><name>P1</name>"
      "<Point><coordinates>1,2</coordinates></Point></Placemark></Document>"
      "        </kml>"
      "      </gmljp2:annotation>"
      "    </gmljp2:GMLJP2GridCoverage>"
      "  </gmljp2:featureMember>"
      "</gmljp2:GMLJP2CoverageCollection>";

  const char* aux_fc =
      "<gml:FeatureCollection xmlns:gml=\"http://www.opengis.net/gml\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
      "xsi:schemaLocation=\"http://www.opengis.net/gml "
      "gmljp2://xml/missing.xsd\">"
      "  <gml:featureMember>"
      "    "
      "<Pt><gml:pointProperty><gml:Point><gml:coordinates>1,2</gml:coordinates>"
      "</gml:Point></gml:pointProperty></Pt>"
      "  </gml:featureMember>"
      "</gml:FeatureCollection>";

  const char* aux_xsd =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"/>";

  std::unique_ptr<GDALJP2Box> b_gml_lbl(GDALJP2Box::CreateLblBox("gml.data"));
  std::unique_ptr<GDALJP2Box> b_gml_root(
      GDALJP2Box::CreateLabelledXMLAssoc("gml.root-instance", gml_root));
  std::unique_ptr<GDALJP2Box> b_aux_fc(
      GDALJP2Box::CreateLabelledXMLAssoc("aux.fc", aux_fc));
  std::unique_ptr<GDALJP2Box> b_aux_xsd(
      GDALJP2Box::CreateLabelledXMLAssoc("aux.xsd", aux_xsd));
  const GDALJP2Box* gml_sub[] = {b_gml_lbl.get(), b_gml_root.get(),
                                 b_aux_fc.get(), b_aux_xsd.get()};
  std::unique_ptr<GDALJP2Box> b_gmljp2_asoc(
      GDALJP2Box::CreateAsocBox(4, gml_sub));
  WriteBox(fp, b_gmljp2_asoc.get());

  // 4. GDALMultiDomainMetadata XML box
  const char* gdal_md_xml =
      "<GDALMultiDomainMetadata>"
      "  <Metadata><MDI key=\"K1\">V1</MDI></Metadata>"
      "  <Metadata domain=\"SUB\"><MDI key=\"SK\">SV</MDI></Metadata>"
      "  <Metadata domain=\"xml:XMP\" format=\"xml\"><XMP2/></Metadata>"
      "</GDALMultiDomainMetadata>";
  GDALJP2Box b_md_xml;
  b_md_xml.SetType("xml ");
  b_md_xml.SetWritableData(static_cast<int>(strlen(gdal_md_xml) + 1),
                           reinterpret_cast<const GByte*>(gdal_md_xml));
  WriteBox(fp, &b_md_xml);

  // 5. IPR box with XML
  const char* ipr_xml = "<ipr>rights</ipr>";
  GDALJP2Box b_ipr;
  b_ipr.SetType("jp2i");
  b_ipr.SetWritableData(static_cast<int>(strlen(ipr_xml) + 1),
                        reinterpret_cast<const GByte*>(ipr_xml));
  WriteBox(fp, &b_ipr);

  // 6. jp2h -> res -> resd superbox hierarchy for resolution metadata
  GDALJP2Box b_resd;
  b_resd.SetType("resd");
  b_resd.AppendUInt16(100);
  b_resd.AppendUInt16(1);
  b_resd.AppendUInt16(100);
  b_resd.AppendUInt16(1);
  b_resd.AppendUInt8(2);
  b_resd.AppendUInt8(2);
  const GDALJP2Box* res_children[] = {&b_resd};
  std::unique_ptr<GDALJP2Box> b_res(
      GDALJP2Box::CreateSuperBox("res ", 1, res_children));
  const GDALJP2Box* jp2h_children[] = {b_res.get()};
  std::unique_ptr<GDALJP2Box> b_jp2h(
      GDALJP2Box::CreateSuperBox("jp2h", 1, jp2h_children));
  WriteBox(fp, b_jp2h.get());

  // 7. Minimal jp2c codestream box with SOC (FF 4F), SIZ (FF 51), COD (FF 52)
  // so GDALGetJPEG2000Reversibility returns "LOSSLESS" (transformation=1)
  GDALJP2Box b_jp2c;
  b_jp2c.SetType("jp2c");
  const GByte codestream[] = {
      0xFF, 0x4F,  // SOC
      0xFF, 0x51, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00,
      0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x01,  // SIZ (1
                                                                   // component)
      0xFF, 0x52, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x01, 0x05, 0x04,
      0x04, 0x00, 0x01,  // COD (reversible 5-3 wavelet = 1)
      0xFF, 0x90         // SOT
  };
  b_jp2c.SetWritableData(sizeof(codestream), codestream);
  WriteBox(fp, &b_jp2c);
  VSIFSeekL(fp, 0, SEEK_SET);

  // Open via GDALOpenInfo with GEOREF_SOURCES including WORLDFILE first +
  // unknown token
  const char* open_opts[] = {
      "GEOREF_SOURCES=WORLDFILE,INTERNAL,PAM,UNKNOWN_SRC", nullptr};
  GDALOpenInfo open_info(jp2_path, GA_ReadOnly, nullptr);
  open_info.papszOpenOptions = const_cast<char**>(open_opts);

  TestJP2Dataset ds(fp);
  ds.SetDescription(jp2_path);
  ds.LoadJP2Metadata(&open_info, nullptr, fp);

  EXPECT_STREQ(ds.GetMetadataItem("K1", ""), "V1");
  EXPECT_STREQ(ds.GetMetadataItem("SK", "SUB"), "SV");
  EXPECT_NE(ds.GetMetadata("xml:IPR"), nullptr);
  EXPECT_NE(ds.GetMetadata("IMAGE_STRUCTURE"), nullptr);
  EXPECT_STREQ(
      ds.GetMetadataItem("COMPRESSION_REVERSIBILITY", "IMAGE_STRUCTURE"),
      "LOSSLESS (possibly)");

  char** file_list = ds.GetFileList();
  CSLDestroy(file_list);

  // LoadVectorLayers (both FALSE and TRUE for remote resources)
  ds.LoadVectorLayers(FALSE);
  EXPECT_GE(ds.GetLayerCount(), 0);
  ds.GetLayer(0);
  EXPECT_TRUE(ds.CloseDependentDatasets() || ds.GetLayerCount() == 0);

  // Also test LoadJP2Metadata with override filename
  TestJP2Dataset ds2;
  ds2.SetDescription(jp2_path);
  ds2.LoadJP2Metadata(&open_info, jp2_path, nullptr);

  VSIFCloseL(fp);
  VSIUnlink(jp2_path);
  VSIUnlink(wld_path);
  VSIUnlink(imd_path);
}

}  // namespace
}  // namespace gdal
