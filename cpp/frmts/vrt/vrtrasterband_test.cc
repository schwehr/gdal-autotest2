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
// Tests for vrtrasterband.cpp.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/vrt_priv.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_hash_set.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class TestVRTRasterBand : public VRTRasterBand {
 public:
  using VRTRasterBand::IsNoDataValueInDataTypeRange;
  using VRTRasterBand::m_bIsMaskBand;

  TestVRTRasterBand(GDALDataset* ds, int band, GDALDataType type = GDT_Byte) {
    Initialize(ds ? ds->GetRasterXSize() : 10, ds ? ds->GetRasterYSize() : 10);
    poDS = ds;
    nBand = band;
    eDataType = type;
  }

  CPLErr IReadBlock(int, int, void* buffer) override {
    memset(buffer, 0,
           nBlockXSize * nBlockYSize * GDALGetDataTypeSizeBytes(eDataType));
    return CE_None;
  }

  CPLErr IWriteBlock(int, int, void*) override { return CE_None; }
};

class VrtRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(VrtRasterBandTest, VRTSerializeNoDataValues) {
  // NaN
  EXPECT_EQ(VRTSerializeNoData(std::numeric_limits<double>::quiet_NaN(),
                               GDT_Float32, 16),
            "nan");

  // Float16 min / max
  EXPECT_EQ(VRTSerializeNoData(-6.55e4, GDT_Float16, 16), "-6.55e4");
  EXPECT_EQ(VRTSerializeNoData(6.55e4, GDT_Float16, 16), "6.55e4");

  // Float32 min / max
  EXPECT_EQ(VRTSerializeNoData(
                -static_cast<double>(std::numeric_limits<float>::max()),
                GDT_Float32, 16),
            "-3.4028234663852886e+38");
  EXPECT_EQ(
      VRTSerializeNoData(static_cast<double>(std::numeric_limits<float>::max()),
                         GDT_Float32, 16),
      "3.4028234663852886e+38");

  // Normal double value
  EXPECT_EQ(VRTSerializeNoData(42.5, GDT_Float64, 6), "42.5");
}

TEST_F(VrtRasterBandTest, VRTParseCategoryNamesAndColorTable) {
  const char* cat_xml =
      "<CategoryNames>"
      "  <Category>Water</Category>"
      "  <Category>Land</Category>"
      "  <Category/>"
      "</CategoryNames>";
  CPLXMLNode* cat_tree = CPLParseXMLString(cat_xml);
  ASSERT_NE(cat_tree, nullptr);
  CPLStringList categories = VRTParseCategoryNames(cat_tree);
  EXPECT_EQ(categories.size(), 3);
  EXPECT_STREQ(categories[0], "Water");
  EXPECT_STREQ(categories[1], "Land");
  EXPECT_STREQ(categories[2], "");
  CPLDestroyXMLNode(cat_tree);

  const char* ct_xml =
      "<ColorTable>"
      "  <Entry c1=\"255\" c2=\"0\" c3=\"0\" c4=\"255\"/>"
      "  <Entry c1=\"0\" c2=\"255\" c3=\"0\" c4=\"255\"/>"
      "</ColorTable>";
  CPLXMLNode* ct_tree = CPLParseXMLString(ct_xml);
  ASSERT_NE(ct_tree, nullptr);
  auto ct = VRTParseColorTable(ct_tree);
  ASSERT_NE(ct, nullptr);
  EXPECT_EQ(ct->GetColorEntryCount(), 2);
  const GDALColorEntry* e0 = ct->GetColorEntry(0);
  ASSERT_NE(e0, nullptr);
  EXPECT_EQ(e0->c1, 255);
  EXPECT_EQ(e0->c2, 0);
  CPLDestroyXMLNode(ct_tree);
}

TEST_F(VrtRasterBandTest, BasicPropertiesAndAccessors) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Float32);

  // UnitType
  EXPECT_STREQ(band.GetUnitType(), "");
  band.SetUnitType("meters");
  EXPECT_STREQ(band.GetUnitType(), "meters");
  band.SetUnitType(nullptr);
  EXPECT_STREQ(band.GetUnitType(), "");

  // Offset & Scale
  int success = 0;
  EXPECT_DOUBLE_EQ(band.GetOffset(&success), 0.0);
  EXPECT_TRUE(success);
  band.SetOffset(10.5);
  EXPECT_DOUBLE_EQ(band.GetOffset(&success), 10.5);

  EXPECT_DOUBLE_EQ(band.GetScale(&success), 1.0);
  band.SetScale(2.5);
  EXPECT_DOUBLE_EQ(band.GetScale(&success), 2.5);

  // Description
  band.SetDescription("My Band");
  EXPECT_STREQ(band.GetDescription(), "My Band");

  // Metadata & HideNoDataValue
  band.SetMetadataItem("HideNoDataValue", "YES");
  char* md[] = {CPLStrdup("FOO=BAR"), nullptr};
  band.SetMetadata(md);
  EXPECT_STREQ(band.GetMetadataItem("FOO"), "BAR");
  CPLFree(md[0]);

  // Color Interpretation
  EXPECT_EQ(band.GetColorInterpretation(), GCI_Undefined);
  band.SetColorInterpretation(GCI_RedBand);
  EXPECT_EQ(band.GetColorInterpretation(), GCI_RedBand);

  // Category Names
  CPLStringList cats;
  cats.AddString("CatA");
  cats.AddString("CatB");
  band.SetCategoryNames(cats.List());
  char** ret_cats = band.GetCategoryNames();
  ASSERT_NE(ret_cats, nullptr);
  EXPECT_STREQ(ret_cats[0], "CatA");
  EXPECT_STREQ(ret_cats[1], "CatB");

  // Color Table
  EXPECT_EQ(band.GetColorTable(), nullptr);
  GDALColorTable ct;
  GDALColorEntry ce = {10, 20, 30, 255};
  ct.SetColorEntry(0, &ce);
  band.SetColorTable(&ct);
  ASSERT_NE(band.GetColorTable(), nullptr);
  EXPECT_EQ(band.GetColorInterpretation(), GCI_PaletteIndex);
  band.SetColorTable(nullptr);
  EXPECT_EQ(band.GetColorTable(), nullptr);

  // RAT
  EXPECT_EQ(band.GetDefaultRAT(), nullptr);
  GDALDefaultRasterAttributeTable rat;
  band.SetDefaultRAT(&rat);
  EXPECT_NE(band.GetDefaultRAT(), nullptr);
  band.SetDefaultRAT(nullptr);
  EXPECT_EQ(band.GetDefaultRAT(), nullptr);
}

TEST_F(VrtRasterBandTest, NoDataValueVariants) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Float32);

  int success = 0;
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  band.SetNoDataValue(-9999.0);
  EXPECT_DOUBLE_EQ(band.GetNoDataValue(&success), -9999.0);
  EXPECT_TRUE(success);
  EXPECT_TRUE(band.IsNoDataValueInDataTypeRange());

  band.DeleteNoDataValue();
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  band.SetNoDataValue(0.0);
  band.UnsetNoDataValue();
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  // HideNoDataValue test
  band.SetNoDataValue(123.0);
  band.SetMetadataItem("HideNoDataValue", "1");
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);
  band.SetMetadataItem("HideNoDataValue", "0");
  band.GetNoDataValue(&success);
  EXPECT_TRUE(success);

  // Int64 NoData
  TestVRTRasterBand int64_band(&ds, 2, GDT_Int64);
  int64_band.SetNoDataValueAsInt64(-123456789012345LL);
  EXPECT_EQ(int64_band.GetNoDataValueAsInt64(&success), -123456789012345LL);
  EXPECT_TRUE(success);
  EXPECT_TRUE(int64_band.IsNoDataValueInDataTypeRange());

  // UInt64 NoData
  TestVRTRasterBand uint64_band(&ds, 3, GDT_UInt64);
  uint64_band.SetNoDataValueAsUInt64(123456789012345ULL);
  EXPECT_EQ(uint64_band.GetNoDataValueAsUInt64(&success), 123456789012345ULL);
  EXPECT_TRUE(success);
  EXPECT_TRUE(uint64_band.IsNoDataValueInDataTypeRange());

  // Calling Int64/UInt64 on wrong type -> error
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.GetNoDataValueAsInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_INT64);
  EXPECT_FALSE(success);
  EXPECT_EQ(int64_band.GetNoDataValueAsUInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64);
  EXPECT_FALSE(success);
  EXPECT_EQ(uint64_band.GetNoDataValueAsInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_INT64);
  EXPECT_FALSE(success);
  CPLPopErrorHandler();
}

TEST_F(VrtRasterBandTest, CopyCommonInfoFromSource) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_drv, nullptr);
  GDALDataset* src_ds = mem_drv->Create("", 10, 10, 1, GDT_Byte, nullptr);
  ASSERT_NE(src_ds, nullptr);

  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(src_band, nullptr);
  src_band->SetDescription("SourceBandDesc");
  src_band->SetNoDataValue(255.0);
  src_band->SetOffset(1.0);
  src_band->SetScale(2.0);
  src_band->SetUnitType("km");
  src_band->SetColorInterpretation(GCI_AlphaBand);
  src_band->SetMetadataItem("NBITS", "4", "IMAGE_STRUCTURE");
  src_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");

  GDALDefaultRasterAttributeTable rat;
  src_band->SetDefaultRAT(&rat);

  VRTDataset vrt_ds(10, 10);
  TestVRTRasterBand dst_band(&vrt_ds, 1, GDT_Byte);
  EXPECT_EQ(dst_band.CopyCommonInfoFrom(src_band), CE_None);

  EXPECT_STREQ(dst_band.GetDescription(), "SourceBandDesc");
  int success = 0;
  EXPECT_DOUBLE_EQ(dst_band.GetNoDataValue(&success), 255.0);
  EXPECT_DOUBLE_EQ(dst_band.GetOffset(), 1.0);
  EXPECT_DOUBLE_EQ(dst_band.GetScale(), 2.0);
  EXPECT_STREQ(dst_band.GetUnitType(), "km");
  EXPECT_EQ(dst_band.GetColorInterpretation(), GCI_AlphaBand);
  EXPECT_STREQ(dst_band.GetMetadataItem("NBITS", "IMAGE_STRUCTURE"), "4");
  EXPECT_STREQ(dst_band.GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"),
               "SIGNEDBYTE");

  GDALClose(src_ds);
}

TEST_F(VrtRasterBandTest, Histograms) {
  VRTDataset ds(10, 10);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  GUIntBig hist[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(band.SetDefaultHistogram(0.0, 10.0, 10, hist), CE_None);

  double dfMin = 0.0;
  double dfMax = 0.0;
  int nBuckets = 0;
  GUIntBig* ret_hist = nullptr;
  EXPECT_EQ(band.GetDefaultHistogram(&dfMin, &dfMax, &nBuckets, &ret_hist,
                                     FALSE, nullptr, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(dfMin, 0.0);
  EXPECT_DOUBLE_EQ(dfMax, 10.0);
  EXPECT_EQ(nBuckets, 10);
  ASSERT_NE(ret_hist, nullptr);
  EXPECT_EQ(ret_hist[0], 1);
  EXPECT_EQ(ret_hist[9], 10);
  CPLFree(ret_hist);

  // Replacing existing histogram
  GUIntBig hist2[10] = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  EXPECT_EQ(band.SetDefaultHistogram(0.0, 10.0, 10, hist2), CE_None);

  // GetHistogram matching saved histogram
  GUIntBig query_hist[10] = {0};
  EXPECT_EQ(band.GetHistogram(0.0, 10.0, 10, query_hist, TRUE, TRUE, nullptr,
                              nullptr),
            CE_None);
  EXPECT_EQ(query_hist[0], 10);
}

TEST_F(VrtRasterBandTest, MaskBandOperations) {
  VRTDataset ds(10, 10);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  EXPECT_EQ(band.GetMaskFlags(), GMF_ALL_VALID);
  EXPECT_NE(band.GetMaskBand(), nullptr);

  // Create band mask band
  EXPECT_EQ(band.CreateMaskBand(0), CE_None);
  EXPECT_EQ(band.GetMaskFlags(), 0);
  ASSERT_NE(band.GetMaskBand(), nullptr);
  EXPECT_TRUE(band.GetMaskBand()->IsMaskBand());

  // Error creating second mask band
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.CreateMaskBand(0), CE_Failure);
  CPLPopErrorHandler();

  // Dataset mask band flag
  VRTDataset ds2(10, 10);
  TestVRTRasterBand band2(&ds2, 1, GDT_Byte);
  EXPECT_EQ(band2.CreateMaskBand(GMF_PER_DATASET), CE_None);
  EXPECT_EQ(band2.GetMaskFlags(), GMF_PER_DATASET);
}

TEST_F(VrtRasterBandTest, XMLInitValidationAndOverviews) {
  VRTDataset ds(10, 10);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);
  VRTMapSharedResources map_resources;

  // Invalid node
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.XMLInit(nullptr, nullptr, map_resources), CE_Failure);
  CPLXMLNode* invalid_node = CPLCreateXMLNode(nullptr, CXT_Text, "Invalid");
  EXPECT_EQ(band.XMLInit(invalid_node, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(invalid_node);

  // Invalid dataType
  const char* bad_type_xml = "<VRTRasterBand dataType=\"InvalidType\"/>";
  CPLXMLNode* bad_type = CPLParseXMLString(bad_type_xml);
  ASSERT_NE(bad_type, nullptr);
  EXPECT_EQ(band.XMLInit(bad_type, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(bad_type);
  CPLPopErrorHandler();

  // Full valid XML with Overviews, ColorTable, RAT, MaskBand
  const char* ovr_file = "/vsimem/ovr_test.bin";
  VSILFILE* fp = VSIFOpenL(ovr_file, "wb");
  if (fp) {
    uint8_t zero = 0;
    VSIFWriteL(&zero, 1, 1, fp);
    VSIFCloseL(fp);
  }

  const char* full_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" blockXSize=\"64\" "
      "blockYSize=\"64\">"
      "  <Description>FullDesc</Description>"
      "  <NoDataValue>-999</NoDataValue>"
      "  <HideNoDataValue>0</HideNoDataValue>"
      "  <UnitType>m</UnitType>"
      "  <Offset>10</Offset>"
      "  <Scale>2</Scale>"
      "  <ColorInterp>Gray</ColorInterp>"
      "  <CategoryNames>"
      "    <Category>A</Category>"
      "  </CategoryNames>"
      "  <ColorTable>"
      "    <Entry c1=\"1\" c2=\"2\" c3=\"3\" c4=\"255\"/>"
      "  </ColorTable>"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/ovr_test.bin</SourceFilename>"
      "    <SourceBand>1</SourceBand>"
      "  </Overview>"
      "  <MaskBand>"
      "    <VRTRasterBand dataType=\"Byte\" subClass=\"VRTSourcedRasterBand\"/>"
      "  </MaskBand>"
      "</VRTRasterBand>";

  CPLXMLNode* tree = CPLParseXMLString(full_xml);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  EXPECT_EQ(band.GetRasterDataType(), GDT_Float32);
  EXPECT_STREQ(band.GetDescription(), "FullDesc");
  EXPECT_STREQ(band.GetUnitType(), "m");
  EXPECT_DOUBLE_EQ(band.GetOffset(), 10.0);
  EXPECT_DOUBLE_EQ(band.GetScale(), 2.0);
  EXPECT_EQ(band.GetOverviewCount(), 1);
  EXPECT_EQ(band.GetMaskFlags(), 0);

  // Serialize to XML
  bool warned = false;
  size_t ram = 0;
  CPLXMLNode* serialized = band.SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(serialized, nullptr);
  char* str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "FullDesc"), nullptr);
  EXPECT_NE(strstr(str, "Overview"), nullptr);
  EXPECT_NE(strstr(str, "MaskBand"), nullptr);
  EXPECT_NE(strstr(str, "CategoryNames"), nullptr);
  EXPECT_NE(strstr(str, "ColorTable"), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);

  // Test GetFileList
  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_GE(file_list_size, 1);
  CSLDestroy(file_list);
  CPLHashSetDestroy(hash_set);

  // Test CloseDependentDatasets
  band.CloseDependentDatasets();

  VSIUnlink(ovr_file);
}

TEST_F(VrtRasterBandTest, OverviewAccessAndDetails) {
  const char* ovr_xml =
      "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>"
      "</VRTDataset>";
  VSIFCloseL(
      VSIFileFromMemBuffer("/vsimem/ovr_target.vrt",
                           reinterpret_cast<GByte*>(const_cast<char*>(ovr_xml)),
                           strlen(ovr_xml), FALSE));

  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  const char* band_xml =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/ovr_target.vrt</SourceFilename>"
      "    <SourceBand>1</SourceBand>"
      "  </Overview>"
      "</VRTRasterBand>";
  CPLXMLNode* tree = CPLParseXMLString(band_xml);
  ASSERT_NE(tree, nullptr);
  VRTMapSharedResources map_resources;
  EXPECT_EQ(band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  EXPECT_EQ(band.GetOverviewCount(), 1);
  EXPECT_EQ(band.GetOverview(-1), nullptr);
  EXPECT_EQ(band.GetOverview(5), nullptr);
  GDALRasterBand* ovr_band = band.GetOverview(0);
  ASSERT_NE(ovr_band, nullptr);
  // Second call returns cached pointer
  EXPECT_EQ(band.GetOverview(0), ovr_band);
  band.CloseDependentDatasets();

  // Test recursive overview detection
  ds.SetDescription("/vsimem/recursive.vrt");
  const char* rec_xml =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/recursive.vrt</SourceFilename>"
      "    <SourceBand>1</SourceBand>"
      "  </Overview>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(rec_xml);
  TestVRTRasterBand rec_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(rec_band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(rec_band.GetOverview(0), nullptr);
  CPLPopErrorHandler();

  // Test invalid overview file
  const char* bad_xml =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/does_not_exist.tif</SourceFilename>"
      "    <SourceBand>1</SourceBand>"
      "  </Overview>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(bad_xml);
  TestVRTRasterBand bad_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(bad_band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(bad_band.GetOverview(0), nullptr);
  CPLPopErrorHandler();

  // Test overview with invalid band
  const char* badband_xml =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/ovr_target.vrt</SourceFilename>"
      "    <SourceBand>99</SourceBand>"
      "  </Overview>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(badband_xml);
  TestVRTRasterBand badband_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(badband_band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(badband_band.GetOverview(0), nullptr);
  CPLPopErrorHandler();

  VSIUnlink("/vsimem/ovr_target.vrt");
}

TEST_F(VrtRasterBandTest, VirtualOverviewsAndDisabledOverviews) {
  VRTDataset ds(40, 40);
  ds.AddBand(GDT_Byte, nullptr);
  VRTRasterBand* band = cpl::down_cast<VRTRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(band, nullptr);

  ds.SetEnableOverviews(false);
  EXPECT_EQ(band->GetOverviewCount(), 0);

  ds.SetEnableOverviews(true);
  CPLSetConfigOption("VRT_VIRTUAL_OVERVIEWS", "YES");
  int anOverviews[] = {2};
  ds.BuildOverviews("NEAREST", 1, anOverviews, 0, nullptr, nullptr, nullptr);
  CPLSetConfigOption("VRT_VIRTUAL_OVERVIEWS", nullptr);

  EXPECT_EQ(band->GetOverviewCount(), 1);
  EXPECT_EQ(band->GetOverview(-1), nullptr);
  EXPECT_EQ(band->GetOverview(10), nullptr);
  GDALRasterBand* ovr = band->GetOverview(0);
  EXPECT_NE(ovr, nullptr);
}

TEST_F(VrtRasterBandTest, XMLInitFailuresAndSubclasses) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  // Missing SourceFilename in Overview
  const char* missing_fn =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview><SourceBand>1</SourceBand></Overview>"
      "</VRTRasterBand>";
  CPLXMLNode* tree = CPLParseXMLString(missing_fn);
  TestVRTRasterBand b1(&ds, 1, GDT_Byte);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b1.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // MEM driver check
  const char* mem_fn =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename>MEM:::123</SourceFilename>"
      "  </Overview>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(mem_fn);
  TestVRTRasterBand b2(&ds, 1, GDT_Byte);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b2.XMLInit(tree, "/vsimem/foo.vrt", map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // Relative path in SourceFilename
  const char* rel_fn =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename relativeToVRT=\"1\">rel.tif</SourceFilename>"
      "  </Overview>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(rel_fn);
  TestVRTRasterBand b3(&ds, 1, GDT_Byte);
  EXPECT_EQ(b3.XMLInit(tree, "/vsimem/foo.vrt", map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  // Mismatching band number
  const char* band_mismatch = "<VRTRasterBand dataType=\"Byte\" band=\"5\"/>";
  tree = CPLParseXMLString(band_mismatch);
  TestVRTRasterBand b4(&ds, 1, GDT_Byte);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b4.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // MaskBand subclasses
  const char* derived_mask =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <MaskBand>"
      "    <VRTRasterBand dataType=\"Byte\" subClass=\"VRTDerivedRasterBand\">"
      "      <PixelFunctionType>near</PixelFunctionType>"
      "    </VRTRasterBand>"
      "  </MaskBand>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(derived_mask);
  TestVRTRasterBand b5(&ds, 1, GDT_Byte);
  EXPECT_EQ(b5.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  const char* raw_mask =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <MaskBand>"
      "    <VRTRasterBand dataType=\"Byte\" subClass=\"VRTRawRasterBand\">"
      "      <SourceFilename relativeToVRT=\"1\">dummy.raw</SourceFilename>"
      "    </VRTRasterBand>"
      "  </MaskBand>"
      "</VRTRasterBand>";

  std::vector<GByte> raw_bytes(400, 0);
  VSIFCloseL(VSIFileFromMemBuffer("/vsimem/dummy.raw", raw_bytes.data(),
                                  raw_bytes.size(), FALSE));

  // Raw raster band disabled at run-time: XMLInit must fail even though the
  // backing file exists.
  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO");
  tree = CPLParseXMLString(raw_mask);
  TestVRTRasterBand b6_disabled(&ds, 1, GDT_Byte);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b6_disabled.XMLInit(tree, "/vsimem", map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // Raw raster band enabled
  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES");
  tree = CPLParseXMLString(raw_mask);
  TestVRTRasterBand b6(&ds, 1, GDT_Byte);
  EXPECT_EQ(b6.XMLInit(tree, "/vsimem", map_resources), CE_None);
  CPLDestroyXMLNode(tree);
  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", nullptr);
  VSIUnlink("/vsimem/dummy.raw");

  const char* warped_mask =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <MaskBand>"
      "    <VRTRasterBand dataType=\"Byte\" subClass=\"VRTWarpedRasterBand\"/>"
      "  </MaskBand>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(warped_mask);
  TestVRTRasterBand b7(&ds, 1, GDT_Byte);
  EXPECT_EQ(b7.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  const char* invalid_mask =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <MaskBand>"
      "    <VRTRasterBand dataType=\"Byte\" subClass=\"UnknownSubClass\"/>"
      "  </MaskBand>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(invalid_mask);
  TestVRTRasterBand b8(&ds, 1, GDT_Byte);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b8.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // MaskBand when dataset mask band already exists
  VRTDataset ds_with_mask(20, 20);
  ds_with_mask.CreateMaskBand(GMF_PER_DATASET);
  tree = CPLParseXMLString(derived_mask);
  TestVRTRasterBand b9(&ds_with_mask, 1, GDT_Byte);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b9.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);
}

TEST_F(VrtRasterBandTest, DatasetMaskBandInteractions) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1);
  EXPECT_EQ(ds.CreateMaskBand(GMF_PER_DATASET), CE_None);

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.CreateMaskBand(0), CE_Failure);
  CPLPopErrorHandler();

  EXPECT_NE(band.GetMaskBand(), nullptr);
  EXPECT_EQ(band.GetMaskFlags(), GMF_PER_DATASET);
}

TEST_F(VrtRasterBandTest, Int64AndUInt64NoDataAndSerialization) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  // Int64 XMLInit
  const char* int64_xml =
      "<VRTRasterBand dataType=\"Int64\" band=\"1\">"
      "  <NoDataValue>-9000000000000000000</NoDataValue>"
      "  <HideNoDataValue>1</HideNoDataValue>"
      "</VRTRasterBand>";
  CPLXMLNode* tree = CPLParseXMLString(int64_xml);
  TestVRTRasterBand int64_band(&ds, 1, GDT_Int64);
  EXPECT_EQ(int64_band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  int success = 0;
  int64_band.GetNoDataValue(&success);
  EXPECT_FALSE(success);  // HideNoDataValue was 1

  bool warned = false;
  size_t ram = 0;
  CPLXMLNode* serialized = int64_band.SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(serialized, nullptr);
  char* str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "-9000000000000000000"), nullptr);
  EXPECT_NE(strstr(str, "<HideNoDataValue>1</HideNoDataValue>"), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);

  // UInt64 XMLInit
  const char* uint64_xml =
      "<VRTRasterBand dataType=\"UInt64\" band=\"1\">"
      "  <NoDataValue>18000000000000000000</NoDataValue>"
      "  <HideNoDataValue>0</HideNoDataValue>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(uint64_xml);
  TestVRTRasterBand uint64_band(&ds, 1, GDT_UInt64);
  EXPECT_EQ(uint64_band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  uint64_band.GetNoDataValue(&success);
  EXPECT_TRUE(success);

  serialized = uint64_band.SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(serialized, nullptr);
  str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "18000000000000000000"), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);

  // Calling GetNoDataValueAsUInt64 on Float32 band
  TestVRTRasterBand f32_band(&ds, 1, GDT_Float32);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  f32_band.GetNoDataValueAsUInt64(&success);
  EXPECT_FALSE(success);
  CPLPopErrorHandler();
}

TEST_F(VrtRasterBandTest, HistogramsAndDefaultHistogram) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  GUIntBig panHist[10] = {0};
  EXPECT_EQ(
      band.GetHistogram(0.0, 10.0, 10, panHist, FALSE, FALSE, nullptr, nullptr),
      CE_None);
  EXPECT_EQ(band.SetDefaultHistogram(0.0, 10.0, 10, panHist), CE_None);

  double min = 0, max = 0;
  int buckets = 0;
  GUIntBig* ret_hist = nullptr;
  EXPECT_EQ(band.GetDefaultHistogram(&min, &max, &buckets, &ret_hist, FALSE,
                                     nullptr, nullptr),
            CE_None);
  EXPECT_EQ(buckets, 10);
  VSIFree(ret_hist);

  bool warned = false;
  size_t ram = 0;
  CPLXMLNode* serialized = band.SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(serialized, nullptr);
  char* str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "Histograms"), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);
}

TEST_F(VrtRasterBandTest, FileListWithNonExistentAndDuplicates) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  const char* fn = "/vsimem/fl_test.bin";
  VSIFCloseL(VSIFileFromMemBuffer(
      fn, reinterpret_cast<GByte*>(const_cast<char*>("1")), 1, FALSE));

  const char* xml =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/fl_test.bin</SourceFilename>"
      "  </Overview>"
      "  <Overview>"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/does_not_exist.bin</SourceFilename>"
      "  </Overview>"
      "</VRTRasterBand>";
  CPLXMLNode* tree = CPLParseXMLString(xml);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);
  EXPECT_EQ(band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);

  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_EQ(file_list_size, 1);

  // Calling again hits duplicate branch
  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_EQ(file_list_size, 1);

  CSLDestroy(file_list);
  CPLHashSetDestroy(hash_set);
  VSIUnlink(fn);
}

TEST_F(VrtRasterBandTest, CategoryNamesAndColorTableExtraChildren) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  const char* xml =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "  <CategoryNames>"
      "    <Invalid>Skip</Invalid>"
      "    <Category>RealCat</Category>"
      "  </CategoryNames>"
      "  <ColorTable>"
      "    <Invalid>Skip</Invalid>"
      "    <Entry c1=\"1\" c2=\"2\" c3=\"3\" c4=\"255\"/>"
      "  </ColorTable>"
      "</VRTRasterBand>";
  CPLXMLNode* tree = CPLParseXMLString(xml);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);
  EXPECT_EQ(band.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  char** cats = band.GetCategoryNames();
  ASSERT_NE(cats, nullptr);
  EXPECT_STREQ(cats[0], "RealCat");
  EXPECT_NE(band.GetColorTable(), nullptr);
}

TEST_F(VrtRasterBandTest, NoDataRangeInfinityAndUnset) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand float_band(&ds, 1, GDT_Float32);
  EXPECT_TRUE(float_band.IsNoDataValueInDataTypeRange());

  float_band.SetNoDataValue(std::numeric_limits<double>::infinity());
  EXPECT_TRUE(float_band.IsNoDataValueInDataTypeRange());

  TestVRTRasterBand byte_band(&ds, 1, GDT_Byte);
  byte_band.SetNoDataValue(std::numeric_limits<double>::infinity());
  EXPECT_FALSE(byte_band.IsNoDataValueInDataTypeRange());
}

}  // namespace
}  // namespace autotest2
