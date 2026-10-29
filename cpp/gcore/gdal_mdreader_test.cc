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

#include "third_party/gdal/gcore/gdal_mdreader.h"

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

void WriteFile(const char* path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(fp, nullptr);
  if (!content.empty()) {
    ASSERT_EQ(VSIFWriteL(content.data(), 1, content.size(), fp),
              content.size());
  }
  ASSERT_EQ(VSIFCloseL(fp), 0);
}

class TestableMDReader : public GDALMDReaderBase {
 public:
  TestableMDReader() : GDALMDReaderBase("/vsimem/test.tif", nullptr) {
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

  std::string twenty_vals =
      "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20";
  md.SetNameValue(RPC_LINE_NUM_COEFF, twenty_vals.c_str());
  md.SetNameValue(RPC_LINE_DEN_COEFF, twenty_vals.c_str());
  md.SetNameValue(RPC_SAMP_NUM_COEFF, twenty_vals.c_str());
  md.SetNameValue(RPC_SAMP_DEN_COEFF, twenty_vals.c_str());
  return md;
}

TEST(GdalMdReaderTest, ManagerAndBaseClassMethods) {
  GDALAllRegister();

  // GDALMDReaderManager:
  {
    GDALMDReaderManager mgr;
    // Non-sidecar path:
    EXPECT_EQ(mgr.GetReader("MEM:::", nullptr, MDR_ANY), nullptr);
    // Sidecar path with no matching metadata files (tests all 11 reader
    // checks):
    WriteFile("/vsimem/no_md_files.tif", "dummy");
    EXPECT_EQ(mgr.GetReader("/vsimem/no_md_files.tif", nullptr, MDR_ANY),
              nullptr);
    VSIUnlink("/vsimem/no_md_files.tif");

    // DigitalGlobe match (creates m_pReader and destroys in
    // ~GDALMDReaderManager):
    WriteFile("/vsimem/dg_scene.tif", "dummy");
    WriteFile("/vsimem/dg_scene.IMD", "version = \"R\";\nEND;\n");
    GDALMDReaderBase* reader =
        mgr.GetReader("/vsimem/dg_scene.tif", nullptr, MDR_DG);
    EXPECT_NE(reader, nullptr);
    VSIUnlink("/vsimem/dg_scene.tif");
    VSIUnlink("/vsimem/dg_scene.IMD");
  }

  // GDALMDReaderBase methods:
  TestableMDReader base;
  base.LoadMetadata();
  base.LoadMetadata();  // second call hits m_bIsMetadataLoad early return.

  EXPECT_NE(base.GetMetadataDomain(MD_DOMAIN_DEFAULT), nullptr);
  EXPECT_NE(base.GetMetadataDomain(MD_DOMAIN_IMD), nullptr);
  EXPECT_NE(base.GetMetadataDomain(MD_DOMAIN_RPC), nullptr);
  EXPECT_NE(base.GetMetadataDomain(MD_DOMAIN_IMAGERY), nullptr);
  EXPECT_EQ(base.GetMetadataDomain("UNKNOWN_DOMAIN"), nullptr);

  EXPECT_EQ(base.GetAcquisitionTimeFromString(nullptr), 0);
  EXPECT_EQ(base.GetAcquisitionTimeFromString("not-a-date"), 0);
  EXPECT_GT(base.GetAcquisitionTimeFromString("2023-05-15T12:34:56.789Z"), 0);

  EXPECT_FALSE(base.FillMetadata(nullptr));
  GDALMultiDomainMetadata mdmd;
  EXPECT_TRUE(base.FillMetadata(&mdmd));
  EXPECT_STREQ(mdmd.GetMetadataItem("IMD_KEY", MD_DOMAIN_IMD), "IMD_VAL");
  EXPECT_STREQ(mdmd.GetMetadataItem("RPC_KEY", MD_DOMAIN_RPC), "RPC_VAL");
  EXPECT_STREQ(mdmd.GetMetadataItem("IMG_KEY", MD_DOMAIN_IMAGERY), "IMG_VAL");
  EXPECT_STREQ(mdmd.GetMetadataItem("DEF_KEY", MD_DOMAIN_DEFAULT), "DEF_VAL");

  // ReadXMLToList:
  EXPECT_EQ(base.CallReadXMLToList(nullptr, nullptr, ""), nullptr);

  const char* xml_str =
      "<Root attr=\"attr_val\">"
      "  <Item id=\"1\">First</Item>"
      "  <Item id=\"2\"><Sub>SubVal</Sub></Item>"
      "</Root>"
      "<SecondRoot>TopText</SecondRoot>";
  CPLXMLNode* xml_node = CPLParseXMLString(xml_str);
  ASSERT_NE(xml_node, nullptr);
  char** xml_list = base.CallReadXMLToList(xml_node, nullptr, "");
  ASSERT_NE(xml_list, nullptr);
  EXPECT_STREQ(CSLFetchNameValue(xml_list, ".attr"), "attr_val");
  EXPECT_STREQ(CSLFetchNameValue(xml_list, "Root.Item_1"), "First");
  EXPECT_STREQ(CSLFetchNameValue(xml_list, "Root.Item_2.Sub"), "SubVal");
  EXPECT_STREQ(CSLFetchNameValue(xml_list, "SecondRoot"), "TopText");
  CSLDestroy(xml_list);
  CPLDestroyXMLNode(xml_node);

  // Deeply nested XML (depth >= 10) error in ReadXMLToListFirstPass:
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const char* deep_xml =
      "<n0><n1><n2><n3><n4><n5><n6><n7><n8><n9><n10>v</n10>"
      "</n9></n8></n7></n6></n5></n4></n3></n2></n1></n0>";
  CPLXMLNode* deep_node = CPLParseXMLString(deep_xml);
  ASSERT_NE(deep_node, nullptr);
  EXPECT_EQ(base.CallReadXMLToList(deep_node, nullptr, ""), nullptr);
  CPLDestroyXMLNode(deep_node);
  CPLPopErrorHandler();
}

TEST(GdalMdReaderTest, MiscAndRPBAndRPCAndIMDFunctions) {
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // GDALCheckFileHeader, CPLStrip, CPLStripQuotes:
  EXPECT_FALSE(GDALCheckFileHeader("/vsimem/no_such_file.xml", "<isd>"));
  WriteFile("/vsimem/empty_header.xml", "");
  EXPECT_FALSE(GDALCheckFileHeader("/vsimem/empty_header.xml", "<isd>"));
  WriteFile("/vsimem/valid_header.xml", "<?xml?><isd>hello</isd>");
  EXPECT_TRUE(GDALCheckFileHeader("/vsimem/valid_header.xml", "<isd>"));
  EXPECT_FALSE(GDALCheckFileHeader("/vsimem/valid_header.xml", "<other>"));
  VSIUnlink("/vsimem/empty_header.xml");
  VSIUnlink("/vsimem/valid_header.xml");

  EXPECT_STREQ(CPLStrip("", '"').c_str(), "");
  EXPECT_STREQ(CPLStrip("\"\"", '"').c_str(), "");
  EXPECT_STREQ(CPLStripQuotes("\"hello\"").c_str(), "hello");
  EXPECT_STREQ(CPLStripQuotes("'world'").c_str(), "world");

  // RPB read/write:
  EXPECT_EQ(GDALLoadRPBFile(""), nullptr);
  EXPECT_EQ(GDALLoadRPBFile("/vsimem/nonexistent.RPB"), nullptr);
  WriteFile("/vsimem/bad_syntax.RPB", "BEGIN_GROUP = IMAGE\n");
  EXPECT_EQ(GDALLoadRPBFile("/vsimem/bad_syntax.RPB"), nullptr);
  WriteFile("/vsimem/incomplete.RPB",
            "BEGIN_GROUP = IMAGE\n\terrBias = 1.0;\nEND_GROUP = IMAGE\nEND;\n");
  EXPECT_EQ(GDALLoadRPBFile("/vsimem/incomplete.RPB"), nullptr);
  VSIUnlink("/vsimem/bad_syntax.RPB");
  VSIUnlink("/vsimem/incomplete.RPB");

  // GDALWriteRPBFile with nullptr unlinks file:
  EXPECT_EQ(GDALWriteRPBFile("/vsimem/test_rpb.tif", nullptr), CE_None);
  // Missing required field:
  CPLStringList incomplete_md;
  incomplete_md.SetNameValue(RPC_ERR_BIAS, "1.0");
  EXPECT_EQ(GDALWriteRPBFile("/vsimem/test_rpb.tif", incomplete_md.List()),
            CE_Failure);

  // Corrupt COEF field (not 20 values):
  CPLStringList corrupt_md = CreateValidRPCMetadata(false);
  corrupt_md.SetNameValue(RPC_LINE_NUM_COEFF, "1 2 3");
  EXPECT_EQ(GDALWriteRPBFile("/vsimem/test_rpb.tif", corrupt_md.List()),
            CE_Failure);

  // Valid RPB write (without ERR_BIAS/ERR_RAND so default 0.0 branch runs) &
  // read:
  CPLStringList valid_md_no_err = CreateValidRPCMetadata(false);
  EXPECT_EQ(GDALWriteRPBFile("/vsimem/test_rpb.tif", valid_md_no_err.List()),
            CE_None);
  char** loaded_rpb = GDALLoadRPBFile("/vsimem/test_rpb.RPB");
  ASSERT_NE(loaded_rpb, nullptr);
  EXPECT_STREQ(CSLFetchNameValue(loaded_rpb, RPC_LINE_OFF), "100");
  CSLDestroy(loaded_rpb);
  VSIUnlink("/vsimem/test_rpb.RPB");

  // RPC (_RPC.TXT) read/write:
  EXPECT_EQ(GDALLoadRPCFile(""), nullptr);
  EXPECT_EQ(GDALLoadRPCFile("/vsimem/nonexistent_rpc.txt"), nullptr);
  WriteFile("/vsimem/missing_scalar_rpc.txt", "ERR_BIAS: 1.0\n");
  EXPECT_EQ(GDALLoadRPCFile("/vsimem/missing_scalar_rpc.txt"), nullptr);
  VSIUnlink("/vsimem/missing_scalar_rpc.txt");

  // Filename without extension fails GDALWriteRPCTXTFile:
  EXPECT_EQ(
      GDALWriteRPCTXTFile("/vsimem/no_dot_filename", valid_md_no_err.List()),
      CE_Failure);
  EXPECT_EQ(GDALWriteRPCTXTFile("/vsimem/test_rpc.tif", nullptr), CE_None);
  EXPECT_EQ(GDALWriteRPCTXTFile("/vsimem/test_rpc.tif", incomplete_md.List()),
            CE_Failure);
  EXPECT_EQ(GDALWriteRPCTXTFile("/vsimem/test_rpc.tif", corrupt_md.List()),
            CE_Failure);
  CPLStringList missing_coeff_md = CreateValidRPCMetadata(true);
  missing_coeff_md.SetNameValue(RPC_LINE_NUM_COEFF, nullptr);
  EXPECT_EQ(
      GDALWriteRPCTXTFile("/vsimem/test_rpc.tif", missing_coeff_md.List()),
      CE_Failure);

  // Valid RPCTXT write & read:
  EXPECT_EQ(GDALWriteRPCTXTFile("/vsimem/test_rpc.tif", valid_md_no_err.List()),
            CE_None);
  char** loaded_rpc = GDALLoadRPCFile("/vsimem/test_rpc_RPC.TXT");
  ASSERT_NE(loaded_rpc, nullptr);
  EXPECT_STREQ(CSLFetchNameValue(loaded_rpc, RPC_SAMP_OFF), "200");
  CSLDestroy(loaded_rpc);

  // Also test GDALLoadRPCFile when a _1 coeff line is missing:
  WriteFile("/vsimem/missing_coeff_rpc.txt",
            "LINE_OFF: 1\nSAMP_OFF: 1\nLAT_OFF: 1\nLONG_OFF: 1\n"
            "HEIGHT_OFF: 1\nLINE_SCALE: 1\nSAMP_SCALE: 1\nLAT_SCALE: 1\n"
            "LONG_SCALE: 1\nHEIGHT_SCALE: 1\n");
  EXPECT_EQ(GDALLoadRPCFile("/vsimem/missing_coeff_rpc.txt"), nullptr);
  VSIUnlink("/vsimem/missing_coeff_rpc.txt");
  VSIUnlink("/vsimem/test_rpc_RPC.TXT");

  // IMD read/write and GDAL_IMD_AA2R conversion:
  EXPECT_EQ(GDALLoadIMDFile(""), nullptr);
  EXPECT_EQ(GDALLoadIMDFile("/vsimem/nonexistent.IMD"), nullptr);
  WriteFile("/vsimem/bad_syntax.IMD", "BEGIN_GROUP = IMAGE_1\n");
  EXPECT_EQ(GDALLoadIMDFile("/vsimem/bad_syntax.IMD"), nullptr);
  VSIUnlink("/vsimem/bad_syntax.IMD");

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

  EXPECT_EQ(GDALWriteIMDFile("/vsimem/test_imd.tif", imd_md.List()), CE_None);
  char** loaded_imd = GDALLoadIMDFile("/vsimem/test_imd.IMD");
  ASSERT_NE(loaded_imd, nullptr);
  // Version "AA" should have been converted to "R" by GDAL_IMD_AA2R:
  EXPECT_STREQ(CSLFetchNameValue(loaded_imd, "version"), "\"R\"");
  // productCatalogId removed:
  EXPECT_EQ(CSLFetchNameValue(loaded_imd, "productCatalogId"), nullptr);
  // meanCollectedRowGSD renamed to collectedRowGSD:
  EXPECT_STREQ(CSLFetchNameValue(loaded_imd, "IMAGE_1.collectedRowGSD"), "1.5");
  CSLDestroy(loaded_imd);
  VSIUnlink("/vsimem/test_imd.IMD");

  CPLPopErrorHandler();
}

}  // namespace
