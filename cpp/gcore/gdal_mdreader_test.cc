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
// Unit tests for gcore/gdal_mdreader.cpp and RP/IMD/XML metadata helpers.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdal_mdreader.cpp

#include "third_party/gdal/gcore/gdal_mdreader.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdal_mdreader_test";

class TestableMDReader final : public GDALMDReaderBase {
 public:
  TestableMDReader()
      : GDALMDReaderBase((std::string(kTestDir) + "/test.tif").c_str(),
                         nullptr) {
    m_papszIMDMD = CSLSetNameValue(m_papszIMDMD, "IMD_KEY", "IMD_VAL");
    m_papszRPCMD = CSLSetNameValue(m_papszRPCMD, "RPC_KEY", "RPC_VAL");
    m_papszIMAGERYMD = CSLSetNameValue(m_papszIMAGERYMD, "IMG_KEY", "IMG_VAL");
    m_papszDEFAULTMD = CSLSetNameValue(m_papszDEFAULTMD, "DEF_KEY", "DEF_VAL");
  }

  bool HasRequiredFiles() const override { return true; }
  char** GetMetadataFiles() const override { return nullptr; }

  using GDALMDReaderBase::GetAcquisitionTimeFromString;
  using GDALMDReaderBase::LoadMetadata;
  char** CallReadXMLToList(CPLXMLNode* psNode, char** papszList,
                           const char* pszName = "") {
    return GDALMDReaderBase::ReadXMLToList(psNode, papszList, pszName);
  }
};

CPLStringList CreateValidRPCMetadata(bool include_err_fields = true) {
  CPLStringList md;
  if (include_err_fields) {
    md.SetNameValue(RPC_ERR_BIAS, "1.5");
    md.SetNameValue(RPC_ERR_RAND, "0.5");
  }
  md.SetNameValue(RPC_LINE_OFF, "100");
  md.SetNameValue(RPC_SAMP_OFF, "200");
  md.SetNameValue(RPC_LAT_OFF, "30.0");
  md.SetNameValue(RPC_LONG_OFF, "40.0");
  md.SetNameValue(RPC_HEIGHT_OFF, "50.0");
  md.SetNameValue(RPC_LINE_SCALE, "100");
  md.SetNameValue(RPC_SAMP_SCALE, "200");
  md.SetNameValue(RPC_LAT_SCALE, "1.0");
  md.SetNameValue(RPC_LONG_SCALE, "1.0");
  md.SetNameValue(RPC_HEIGHT_SCALE, "10.0");

  constexpr char kTwentyVals[] =
      "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20";
  md.SetNameValue(RPC_LINE_NUM_COEFF, kTwentyVals);
  md.SetNameValue(RPC_LINE_DEN_COEFF, kTwentyVals);
  md.SetNameValue(RPC_SAMP_NUM_COEFF, kTwentyVals);
  md.SetNameValue(RPC_SAMP_DEN_COEFF, kTwentyVals);
  return md;
}

TEST(GdalMdReaderTest, ManagerAndBaseClassMethods) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  EXPECT_EQ(0, VSIMkdir(kTestDir, 0755));

  // GDALMDReaderManager:
  {
    GDALMDReaderManager mgr;
    // Non-sidecar path:
    EXPECT_EQ(nullptr, mgr.GetReader("MEM:::", nullptr, MDR_ANY));
    // Sidecar path with no matching metadata files (tests all reader checks):
    {
      const std::string no_md_path = std::string(kTestDir) + "/no_md_files.tif";
      VsiMemTempWrapper dummy_tif(no_md_path, "dummy");
      EXPECT_EQ(nullptr, mgr.GetReader(no_md_path.c_str(), nullptr, MDR_ANY));
    }

    // DigitalGlobe match (creates m_pReader and destroys in
    // ~GDALMDReaderManager):
    {
      const std::string dg_tif = std::string(kTestDir) + "/dg_scene.tif";
      const std::string dg_imd = std::string(kTestDir) + "/dg_scene.IMD";
      VsiMemTempWrapper tif_file(dg_tif, "dummy");
      VsiMemTempWrapper imd_file(dg_imd, "version = \"R\";\nEND;\n");
      GDALMDReaderBase* const reader =
          mgr.GetReader(dg_tif.c_str(), nullptr, MDR_DG);
      EXPECT_NE(nullptr, reader);
    }
  }

  // GDALMDReaderBase methods:
  TestableMDReader base;
  base.LoadMetadata();
  base.LoadMetadata();  // second call hits m_bIsMetadataLoad early return.

  EXPECT_NE(nullptr, base.GetMetadataDomain(MD_DOMAIN_DEFAULT));
  EXPECT_NE(nullptr, base.GetMetadataDomain(MD_DOMAIN_IMD));
  EXPECT_NE(nullptr, base.GetMetadataDomain(MD_DOMAIN_RPC));
  EXPECT_NE(nullptr, base.GetMetadataDomain(MD_DOMAIN_IMAGERY));
  EXPECT_EQ(nullptr, base.GetMetadataDomain("UNKNOWN_DOMAIN"));

  EXPECT_EQ(0, base.GetAcquisitionTimeFromString(nullptr));
  EXPECT_EQ(0, base.GetAcquisitionTimeFromString("not-a-date"));
  EXPECT_GT(base.GetAcquisitionTimeFromString("2023-05-15T12:34:56.789Z"), 0);

  EXPECT_FALSE(base.FillMetadata(nullptr));
  GDALMultiDomainMetadata mdmd;
  EXPECT_TRUE(base.FillMetadata(&mdmd));
  EXPECT_STREQ("IMD_VAL", mdmd.GetMetadataItem("IMD_KEY", MD_DOMAIN_IMD));
  EXPECT_STREQ("RPC_VAL", mdmd.GetMetadataItem("RPC_KEY", MD_DOMAIN_RPC));
  EXPECT_STREQ("IMG_VAL", mdmd.GetMetadataItem("IMG_KEY", MD_DOMAIN_IMAGERY));
  EXPECT_STREQ("DEF_VAL", mdmd.GetMetadataItem("DEF_KEY", MD_DOMAIN_DEFAULT));

  // ReadXMLToList:
  EXPECT_EQ(nullptr, base.CallReadXMLToList(nullptr, nullptr, ""));

  constexpr char kXmlStr[] =
      "<Root attr=\"attr_val\">"
      "  <Item id=\"1\">First</Item>"
      "  <Item id=\"2\"><Sub>SubVal</Sub></Item>"
      "</Root>"
      "<SecondRoot>TopText</SecondRoot>";
  const CPLXMLTreeCloser xml_node(CPLParseXMLString(kXmlStr));
  ASSERT_NE(nullptr, xml_node);
  const StringListPtr xml_list(base.CallReadXMLToList(xml_node.get(), nullptr),
                               CSLDestroy);
  ASSERT_NE(nullptr, xml_list);
  EXPECT_STREQ("attr_val", CSLFetchNameValue(xml_list.get(), ".attr"));
  EXPECT_STREQ("First", CSLFetchNameValue(xml_list.get(), "Root.Item_1"));
  EXPECT_STREQ("SubVal", CSLFetchNameValue(xml_list.get(), "Root.Item_2.Sub"));
  EXPECT_STREQ("TopText", CSLFetchNameValue(xml_list.get(), "SecondRoot"));

  // Deeply nested XML (depth >= 10) error in ReadXMLToListFirstPass:
  constexpr char kDeepXml[] =
      "<n0><n1><n2><n3><n4><n5><n6><n7><n8><n9><n10>v</n10>"
      "</n9></n8></n7></n6></n5></n4></n3></n2></n1></n0>";
  const CPLXMLTreeCloser deep_node(CPLParseXMLString(kDeepXml));
  ASSERT_NE(nullptr, deep_node);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const StringListPtr deep_list(
        base.CallReadXMLToList(deep_node.get(), nullptr, ""), CSLDestroy);
    EXPECT_EQ(nullptr, deep_list);
    CPLErrorReset();
  }
}

TEST(GdalMdReaderTest, MiscAndRPBAndRPCAndIMDFunctions) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  EXPECT_EQ(0, VSIMkdir(kTestDir, 0755));

  // GDALCheckFileHeader, CPLStrip, CPLStripQuotes:
  const std::string missing_xml = std::string(kTestDir) + "/no_such_file.xml";
  EXPECT_FALSE(GDALCheckFileHeader(missing_xml.c_str(), "<isd>"));
  {
    const std::string empty_xml = std::string(kTestDir) + "/empty_header.xml";
    const std::string valid_xml = std::string(kTestDir) + "/valid_header.xml";
    VsiMemTempWrapper empty_file(empty_xml, "");
    EXPECT_FALSE(GDALCheckFileHeader(empty_xml.c_str(), "<isd>"));
    VsiMemTempWrapper valid_file(valid_xml, "<?xml?><isd>hello</isd>");
    EXPECT_TRUE(GDALCheckFileHeader(valid_xml.c_str(), "<isd>"));
    EXPECT_FALSE(GDALCheckFileHeader(valid_xml.c_str(), "<other>"));
  }

  EXPECT_EQ("", CPLStrip("", '"'));
  EXPECT_EQ("", CPLStrip("\"\"", '"'));
  EXPECT_EQ("hello", CPLStripQuotes("\"hello\""));
  EXPECT_EQ("world", CPLStripQuotes("'world'"));

  // RPB read/write:
  EXPECT_EQ(nullptr, GDALLoadRPBFile(""));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string missing_rpb = std::string(kTestDir) + "/nonexistent.RPB";
    EXPECT_EQ(nullptr, GDALLoadRPBFile(missing_rpb.c_str()));

    const std::string bad_rpb = std::string(kTestDir) + "/bad_syntax.RPB";
    VsiMemTempWrapper bad_file(bad_rpb, "BEGIN_GROUP = IMAGE\n");
    EXPECT_EQ(nullptr, GDALLoadRPBFile(bad_rpb.c_str()));

    const std::string inc_rpb = std::string(kTestDir) + "/incomplete.RPB";
    VsiMemTempWrapper inc_file(
        inc_rpb,
        "BEGIN_GROUP = IMAGE\n\terrBias = 1.0;\nEND_GROUP = IMAGE\nEND;\n");
    EXPECT_EQ(nullptr, GDALLoadRPBFile(inc_rpb.c_str()));
    CPLErrorReset();
  }

  // GDALWriteRPBFile with nullptr unlinks file:
  const std::string test_rpb_base = std::string(kTestDir) + "/test_rpb.tif";
  EXPECT_EQ(CE_None, GDALWriteRPBFile(test_rpb_base.c_str(), nullptr));

  // Missing required field & corrupt COEF field:
  CPLStringList incomplete_md;
  incomplete_md.SetNameValue(RPC_ERR_BIAS, "1.0");
  CPLStringList corrupt_md = CreateValidRPCMetadata(false);
  corrupt_md.SetNameValue(RPC_LINE_NUM_COEFF, "1 2 3");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALWriteRPBFile(test_rpb_base.c_str(), incomplete_md.List()));
    EXPECT_EQ(CE_Failure,
              GDALWriteRPBFile(test_rpb_base.c_str(), corrupt_md.List()));
    CPLErrorReset();
  }

  // Valid RPB write (without ERR_BIAS/ERR_RAND so default 0.0 branch runs) &
  // read:
  CPLStringList valid_md_no_err = CreateValidRPCMetadata(false);
  EXPECT_EQ(CE_None,
            GDALWriteRPBFile(test_rpb_base.c_str(), valid_md_no_err.List()));
  const std::string written_rpb = std::string(kTestDir) + "/test_rpb.RPB";
  const StringListPtr loaded_rpb(GDALLoadRPBFile(written_rpb.c_str()),
                                 CSLDestroy);
  ASSERT_NE(nullptr, loaded_rpb);
  EXPECT_STREQ("100", CSLFetchNameValue(loaded_rpb.get(), RPC_LINE_OFF));

  // RPC (_RPC.TXT) read/write:
  EXPECT_EQ(nullptr, GDALLoadRPCFile(""));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string missing_rpc =
        std::string(kTestDir) + "/nonexistent_rpc.txt";
    EXPECT_EQ(nullptr, GDALLoadRPCFile(missing_rpc.c_str()));

    const std::string missing_scalar_rpc =
        std::string(kTestDir) + "/missing_scalar_rpc.txt";
    VsiMemTempWrapper scalar_file(missing_scalar_rpc, "ERR_BIAS: 1.0\n");
    EXPECT_EQ(nullptr, GDALLoadRPCFile(missing_scalar_rpc.c_str()));
    CPLErrorReset();
  }

  // Filename without extension and invalid metadata fail GDALWriteRPCTXTFile:
  const std::string no_dot_rpc = std::string(kTestDir) + "/no_dot_filename";
  const std::string test_rpc_base = std::string(kTestDir) + "/test_rpc.tif";
  CPLStringList missing_coeff_md = CreateValidRPCMetadata(true);
  missing_coeff_md.SetNameValue(RPC_LINE_NUM_COEFF, nullptr);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALWriteRPCTXTFile(no_dot_rpc.c_str(), valid_md_no_err.List()));
    EXPECT_EQ(CE_Failure,
              GDALWriteRPCTXTFile(test_rpc_base.c_str(), incomplete_md.List()));
    EXPECT_EQ(CE_Failure,
              GDALWriteRPCTXTFile(test_rpc_base.c_str(), corrupt_md.List()));
    EXPECT_EQ(CE_Failure, GDALWriteRPCTXTFile(test_rpc_base.c_str(),
                                              missing_coeff_md.List()));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, GDALWriteRPCTXTFile(test_rpc_base.c_str(), nullptr));

  // Valid RPCTXT write & read:
  EXPECT_EQ(CE_None,
            GDALWriteRPCTXTFile(test_rpc_base.c_str(), valid_md_no_err.List()));
  const std::string written_rpc = std::string(kTestDir) + "/test_rpc_RPC.TXT";
  const StringListPtr loaded_rpc(GDALLoadRPCFile(written_rpc.c_str()),
                                 CSLDestroy);
  ASSERT_NE(nullptr, loaded_rpc);
  EXPECT_STREQ("200", CSLFetchNameValue(loaded_rpc.get(), RPC_SAMP_OFF));

  // Test GDALLoadRPCFile when a _1 coeff line is missing:
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string missing_coeff_rpc =
        std::string(kTestDir) + "/missing_coeff_rpc.txt";
    VsiMemTempWrapper coeff_file(
        missing_coeff_rpc,
        "LINE_OFF: 1\nSAMP_OFF: 1\nLAT_OFF: 1\nLONG_OFF: 1\n"
        "HEIGHT_OFF: 1\nLINE_SCALE: 1\nSAMP_SCALE: 1\nLAT_SCALE: 1\n"
        "LONG_SCALE: 1\nHEIGHT_SCALE: 1\n");
    EXPECT_EQ(nullptr, GDALLoadRPCFile(missing_coeff_rpc.c_str()));
    CPLErrorReset();
  }

  // IMD read/write and GDAL_IMD_AA2R conversion:
  EXPECT_EQ(nullptr, GDALLoadIMDFile(""));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string missing_imd = std::string(kTestDir) + "/nonexistent.IMD";
    EXPECT_EQ(nullptr, GDALLoadIMDFile(missing_imd.c_str()));

    const std::string bad_imd = std::string(kTestDir) + "/bad_syntax.IMD";
    VsiMemTempWrapper bad_imd_file(bad_imd, "BEGIN_GROUP = IMAGE_1\n");
    EXPECT_EQ(nullptr, GDALLoadIMDFile(bad_imd.c_str()));
    CPLErrorReset();
  }

  CPLStringList imd_md;
  imd_md.AddString("malformed_entry_without_equals");
  imd_md.SetNameValue("version", "\"AA\"");
  imd_md.SetNameValue("productCatalogId", "cat123");
  imd_md.SetNameValue("mode", "PAN");
  imd_md.SetNameValue("spaceKey", "val with space");
  imd_md.SetNameValue("singleQuoteKey", "val'quote");
  imd_md.SetNameValue("doubleQuoteKey", "val\"quote");
  imd_md.SetNameValue("bothQuotesKey", "val'and\"quote");
  imd_md.SetNameValue("multiLineKey", "( 10, 20, 30 )");
  imd_md.SetNameValue("IMAGE_1.minCollectedRowGSD", "1.0");
  imd_md.SetNameValue("IMAGE_1.maxCollectedRowGSD", "2.0");
  imd_md.SetNameValue("IMAGE_1.meanCollectedRowGSD", "1.5");
  imd_md.SetNameValue("BAND_P.ULLon", "45.0");

  const std::string test_imd_base = std::string(kTestDir) + "/test_imd.tif";
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALWriteIMDFile(test_imd_base.c_str(), imd_md.List()));
    CPLErrorReset();
  }
  const std::string written_imd = std::string(kTestDir) + "/test_imd.IMD";
  const StringListPtr loaded_imd(GDALLoadIMDFile(written_imd.c_str()),
                                 CSLDestroy);
  ASSERT_NE(nullptr, loaded_imd);
  // Version "AA" should have been converted to "R" by GDAL_IMD_AA2R:
  EXPECT_STREQ("\"R\"", CSLFetchNameValue(loaded_imd.get(), "version"));
  // productCatalogId removed:
  EXPECT_EQ(nullptr, CSLFetchNameValue(loaded_imd.get(), "productCatalogId"));
  // meanCollectedRowGSD renamed to collectedRowGSD:
  EXPECT_STREQ("1.5",
               CSLFetchNameValue(loaded_imd.get(), "IMAGE_1.collectedRowGSD"));
}

}  // namespace
}  // namespace autotest2
