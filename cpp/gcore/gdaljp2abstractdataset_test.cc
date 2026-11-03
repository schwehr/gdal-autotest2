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
// Unit tests for gcore/gdaljp2abstractdataset.cpp.
//
// Build caveat in google3: //third_party/gdal/BUILD compiles without libcurl
// (HAVE_CURL undefined), so remote xlink hrefs in GMLJP2 XML fail fast offline.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaljp2abstractdataset.cpp

#include "third_party/gdal/gcore/gdaljp2abstractdataset.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdaljp2metadata.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

constexpr char kTestDir[] = "/vsimem/gdaljp2abstractdataset_test";

class TestJP2Dataset final : public GDALJP2AbstractDataset {
 public:
  explicit TestJP2Dataset(VSILFILE* fp = nullptr) : m_fp(fp) {
    nRasterXSize = 10;
    nRasterYSize = 10;
  }

  using GDALJP2AbstractDataset::CloseDependentDatasets;

 protected:
  VSILFILE* GetFileHandle() override {
    if (m_fp != nullptr) {
      return m_fp;
    }
    return GDALJP2AbstractDataset::GetFileHandle();
  }

 private:
  VSILFILE* m_fp = nullptr;
};

void WriteBox(VSILFILE* fp, const GDALJP2Box* box) {
  ASSERT_NE(nullptr, box);
  std::unique_ptr<GByte, CplFreeDeleter> data(box->GetWritableBoxData());
  ASSERT_NE(nullptr, data);
  const size_t length = static_cast<size_t>(box->GetBoxLength());
  EXPECT_EQ(length, VSIFWriteL(data.get(), 1, length, fp));
}

TEST(GdalJp2AbstractDatasetTest, LoadMetadataAndVectorLayers) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  EXPECT_EQ(0, VSIMkdir(kTestDir, 0755));

  GDALRegister_MEM();
  RegisterOGRGML();
  RegisterOGRKML();
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("MEM"));
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("GML"));
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("KML"));

  const std::string jp2_path = std::string(kTestDir) + "/test_abstract.jp2";
  const std::string wld_path = std::string(kTestDir) + "/test_abstract.wld";
  const std::string imd_path = std::string(kTestDir) + "/test_abstract.imd";

  // Create a worldfile and a DigitalGlobe .imd metadata file
  VsiMemTempWrapper wld_file(wld_path, "1.0\n0.0\n0.0\n-1.0\n100.5\n200.5\n");
  VsiMemTempWrapper imd_file(
      imd_path,
      "version = \"Q\";\nBEGIN_GROUP = IMAGE_1\n\tsatId = "
      "\"WV02\";\nEND_GROUP = IMAGE_1\nEND;\n");

  // Build JP2 boxes in /vsimem/gdaljp2abstractdataset_test/test_abstract.jp2
  VSILFILE* fp = VSIFOpenL(jp2_path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp]() { VSIFCloseL(fp); };

  // 0. JP2 Signature box ("jP  " + 0x0D0A870A) so GDALGetJPEG2000Structure
  // recognizes it
  GDALJP2Box b_sig;
  b_sig.SetType("jP  ");
  constexpr auto sig_data = std::to_array<GByte>({0x0D, 0x0A, 0x87, 0x0A});
  b_sig.SetWritableData(sig_data.size(), sig_data.data());
  WriteBox(fp, &b_sig);

  // 1. GeoJP2 UUID box (with SRS, GeoTransform, GCPs, RPC, PixelIsPoint)
  GDALJP2Metadata jp2_md;
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  jp2_md.SetSpatialRef(&srs);
  const GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  jp2_md.SetGeoTransform(gt);
  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  {
    absl::Cleanup deinit_gcp = [&gcp]() { GDALDeinitGCPs(1, &gcp); };
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 1.0;
    jp2_md.SetGCPs(1, &gcp);
  }
  constexpr auto rpc_md = std::to_array<const char*>(
      {"LINE_OFF=1", "LINE_SCALE=1", "SAMP_OFF=1", "SAMP_SCALE=1", "LAT_OFF=1",
       "LONG_OFF=1", "HEIGHT_OFF=1", "LAT_SCALE=1", "LONG_SCALE=1",
       "HEIGHT_SCALE=1",
       "LINE_NUM_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
       "LINE_DEN_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
       "SAMP_NUM_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1",
       "SAMP_DEN_COEFF=1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1", nullptr});
  jp2_md.SetRPCMD(const_cast<char**>(rpc_md.data()));
  jp2_md.bPixelIsPoint = true;
  std::unique_ptr<GDALJP2Box> geo_box(jp2_md.CreateJP2GeoTIFF());
  ASSERT_NE(nullptr, geo_box);
  WriteBox(fp, geo_box.get());

  // 2. XMP UUID box
  constexpr auto uuid_xmp =
      std::to_array<GByte>({0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                            0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC});
  constexpr char xmp_xml[] = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";
  std::unique_ptr<GDALJP2Box> xmp_box(GDALJP2Box::CreateUUIDBox(
      uuid_xmp.data(), static_cast<int>(std::strlen(xmp_xml) + 1),
      reinterpret_cast<const GByte*>(xmp_xml)));
  WriteBox(fp, xmp_box.get());

  // 3. Labelled XML assoc boxes: gml.root-instance, aux.fc, aux.xsd, and
  // GDALMultiDomainMetadata
  constexpr char gml_root[] =
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

  constexpr char aux_fc[] =
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

  constexpr char aux_xsd[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"/>";

  std::unique_ptr<GDALJP2Box> b_gml_lbl(GDALJP2Box::CreateLblBox("gml.data"));
  std::unique_ptr<GDALJP2Box> b_gml_root(
      GDALJP2Box::CreateLabelledXMLAssoc("gml.root-instance", gml_root));
  std::unique_ptr<GDALJP2Box> b_aux_fc(
      GDALJP2Box::CreateLabelledXMLAssoc("aux.fc", aux_fc));
  std::unique_ptr<GDALJP2Box> b_aux_xsd(
      GDALJP2Box::CreateLabelledXMLAssoc("aux.xsd", aux_xsd));
  const std::array<const GDALJP2Box*, 4> gml_sub = {
      b_gml_lbl.get(), b_gml_root.get(), b_aux_fc.get(), b_aux_xsd.get()};
  std::unique_ptr<GDALJP2Box> b_gmljp2_asoc(
      GDALJP2Box::CreateAsocBox(gml_sub.size(), gml_sub.data()));
  WriteBox(fp, b_gmljp2_asoc.get());

  // 4. GDALMultiDomainMetadata XML box
  constexpr char gdal_md_xml[] =
      "<GDALMultiDomainMetadata>"
      "  <Metadata><MDI key=\"K1\">V1</MDI></Metadata>"
      "  <Metadata domain=\"SUB\"><MDI key=\"SK\">SV</MDI></Metadata>"
      "  <Metadata domain=\"xml:XMP\" format=\"xml\"><XMP2/></Metadata>"
      "</GDALMultiDomainMetadata>";
  GDALJP2Box b_md_xml;
  b_md_xml.SetType("xml ");
  b_md_xml.SetWritableData(static_cast<int>(std::strlen(gdal_md_xml) + 1),
                           reinterpret_cast<const GByte*>(gdal_md_xml));
  WriteBox(fp, &b_md_xml);

  // 5. IPR box with XML
  constexpr char ipr_xml[] = "<ipr>rights</ipr>";
  GDALJP2Box b_ipr;
  b_ipr.SetType("jp2i");
  b_ipr.SetWritableData(static_cast<int>(std::strlen(ipr_xml) + 1),
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
  const std::array<const GDALJP2Box*, 1> res_children = {&b_resd};
  std::unique_ptr<GDALJP2Box> b_res(GDALJP2Box::CreateSuperBox(
      "res ", res_children.size(), res_children.data()));
  const std::array<const GDALJP2Box*, 1> jp2h_children = {b_res.get()};
  std::unique_ptr<GDALJP2Box> b_jp2h(GDALJP2Box::CreateSuperBox(
      "jp2h", jp2h_children.size(), jp2h_children.data()));
  WriteBox(fp, b_jp2h.get());

  // 7. Minimal jp2c codestream box with SOC (FF 4F), SIZ (FF 51), COD (FF 52)
  // so GDALGetJPEG2000Reversibility returns "LOSSLESS" (transformation=1)
  GDALJP2Box b_jp2c;
  b_jp2c.SetType("jp2c");
  constexpr auto codestream = std::to_array<GByte>({
      0xFF, 0x4F,  // SOC
      0xFF, 0x51, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00,
      0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x01,  // SIZ (1
                                                 // component)
      0xFF, 0x52, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x01, 0x05, 0x04, 0x04,
      0x00, 0x01,  // COD (reversible 5-3 wavelet = 1)
      0xFF, 0x90   // SOT
  });
  b_jp2c.SetWritableData(codestream.size(), codestream.data());
  WriteBox(fp, &b_jp2c);
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));

  // Open via GDALOpenInfo with GEOREF_SOURCES including WORLDFILE first +
  // unknown token
  constexpr auto open_opts = std::to_array<const char*>(
      {"GEOREF_SOURCES=WORLDFILE,INTERNAL,PAM,UNKNOWN_SRC", nullptr});
  GDALOpenInfo open_info(jp2_path.c_str(), GA_ReadOnly, nullptr);
  open_info.papszOpenOptions = const_cast<char**>(open_opts.data());

  TestJP2Dataset ds(fp);
  ds.SetDescription(jp2_path.c_str());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ds.LoadJP2Metadata(&open_info, nullptr, fp);
    CPLErrorReset();
  }

  GDALGeoTransform out_gt;
  ASSERT_EQ(CE_None, ds.GetGeoTransform(out_gt));
  EXPECT_DOUBLE_EQ(100.0, out_gt[0]);
  EXPECT_DOUBLE_EQ(1.0, out_gt[1]);
  EXPECT_DOUBLE_EQ(0.0, out_gt[2]);
  EXPECT_DOUBLE_EQ(201.0, out_gt[3]);
  EXPECT_DOUBLE_EQ(0.0, out_gt[4]);
  EXPECT_DOUBLE_EQ(-1.0, out_gt[5]);
  EXPECT_NE(nullptr, ds.GetSpatialRef());
  // WORLDFILE geotransform takes precedence and the fixture defines no GCPs.
  EXPECT_EQ(0, ds.GetGCPCount());
  EXPECT_STREQ("1", ds.GetMetadataItem("LINE_OFF", "RPC"));

  EXPECT_STREQ("V1", ds.GetMetadataItem("K1", ""));
  EXPECT_STREQ("SV", ds.GetMetadataItem("SK", "SUB"));
  EXPECT_NE(nullptr, ds.GetMetadata("xml:IPR"));
  EXPECT_NE(nullptr, ds.GetMetadata("IMAGE_STRUCTURE"));
  EXPECT_STREQ(
      "LOSSLESS (possibly)",
      ds.GetMetadataItem("COMPRESSION_REVERSIBILITY", "IMAGE_STRUCTURE"));

  const StringListPtr file_list(ds.GetFileList(), CSLDestroy);
  EXPECT_THAT(CslToVector(file_list.get()),
              UnorderedElementsAre(jp2_path, wld_path, imd_path));

  // LoadVectorLayers
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ds.LoadVectorLayers(FALSE);
    CPLErrorReset();
  }
  EXPECT_GT(ds.GetLayerCount(), 0);
  EXPECT_NE(nullptr, ds.GetLayer(0));
  EXPECT_TRUE(ds.CloseDependentDatasets());

  // Also test LoadJP2Metadata with override filename
  TestJP2Dataset ds2;
  ds2.SetDescription(jp2_path.c_str());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ds2.LoadJP2Metadata(&open_info, jp2_path.c_str(), nullptr);
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
