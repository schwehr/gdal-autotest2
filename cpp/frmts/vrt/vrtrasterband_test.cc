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
// Tests VRTRasterBand properties, nodata handling, histograms, mask bands,
// overviews, and XML serialization (vrtrasterband.cpp).
//
// google3 build caveat: //third_party/gdal/BUILD passes
// -DGDAL_VRT_ENABLE_RAWRASTERBAND=ON (see b/429626570), so raw-raster-band
// enable/disable tests explicitly toggle GDAL_VRT_ENABLE_RAWRASTERBAND at
// runtime via CPLConfigOptionSetter.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtrasterband.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
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

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtrasterband_test";

class TestVRTRasterBand : public VRTRasterBand {
 public:
  using VRTRasterBand::IsNoDataValueInDataTypeRange;
  using VRTRasterBand::m_bIsMaskBand;

  TestVRTRasterBand(GDALDataset* ds, int band, GDALDataType type = GDT_Byte) {
    Initialize(ds != nullptr ? ds->GetRasterXSize() : 10,
               ds != nullptr ? ds->GetRasterYSize() : 10);
    poDS = ds;
    nBand = band;
    eDataType = type;
  }

  CPLErr IReadBlock(int, int, void* buffer) override {
    std::memset(buffer, 0,
                static_cast<size_t>(nBlockXSize) * nBlockYSize *
                    GDALGetDataTypeSizeBytes(eDataType));
    return CE_None;
  }

  CPLErr IWriteBlock(int, int, void*) override { return CE_None; }
};

class VrtRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_MEM();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(VrtRasterBandTest, VRTSerializeNoDataValues) {
  EXPECT_EQ("nan", VRTSerializeNoData(std::numeric_limits<double>::quiet_NaN(),
                                      GDT_Float32, 16));

  EXPECT_EQ("-6.55e4", VRTSerializeNoData(-6.55e4, GDT_Float16, 16));
  EXPECT_EQ("6.55e4", VRTSerializeNoData(6.55e4, GDT_Float16, 16));

  EXPECT_EQ("-3.4028234663852886e+38",
            VRTSerializeNoData(
                -static_cast<double>(std::numeric_limits<float>::max()),
                GDT_Float32, 16));
  EXPECT_EQ(
      "3.4028234663852886e+38",
      VRTSerializeNoData(static_cast<double>(std::numeric_limits<float>::max()),
                         GDT_Float32, 16));

  EXPECT_EQ("42.5", VRTSerializeNoData(42.5, GDT_Float64, 6));
}

TEST_F(VrtRasterBandTest, VRTParseCategoryNamesAndColorTable) {
  constexpr char kCatXml[] = R"xml(
<CategoryNames>
  <Category>Water</Category>
  <Category>Land</Category>
  <Category/>
</CategoryNames>)xml";
  CPLXMLTreeCloser cat_tree(CPLParseXMLString(kCatXml));
  ASSERT_NE(nullptr, cat_tree.get());
  CPLStringList categories = VRTParseCategoryNames(cat_tree.get());
  EXPECT_EQ(3, categories.size());
  EXPECT_STREQ("Water", categories[0]);
  EXPECT_STREQ("Land", categories[1]);
  EXPECT_STREQ("", categories[2]);

  constexpr char kColorTableXml[] = R"xml(
<ColorTable>
  <Entry c1="255" c2="0" c3="0" c4="255"/>
  <Entry c1="0" c2="255" c3="0" c4="255"/>
</ColorTable>)xml";
  CPLXMLTreeCloser ct_tree(CPLParseXMLString(kColorTableXml));
  ASSERT_NE(nullptr, ct_tree.get());
  auto ct = VRTParseColorTable(ct_tree.get());
  ASSERT_NE(nullptr, ct);
  EXPECT_EQ(2, ct->GetColorEntryCount());
  const GDALColorEntry* e0 = ct->GetColorEntry(0);
  ASSERT_NE(nullptr, e0);
  EXPECT_EQ(255, e0->c1);
  EXPECT_EQ(0, e0->c2);
}

TEST_F(VrtRasterBandTest, BasicPropertiesAndAccessors) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Float32);

  EXPECT_STREQ("", band.GetUnitType());
  band.SetUnitType("meters");
  EXPECT_STREQ("meters", band.GetUnitType());
  band.SetUnitType(nullptr);
  EXPECT_STREQ("", band.GetUnitType());

  int success = 0;
  EXPECT_DOUBLE_EQ(0.0, band.GetOffset(&success));
  EXPECT_TRUE(success);
  band.SetOffset(10.5);
  EXPECT_DOUBLE_EQ(10.5, band.GetOffset(&success));

  EXPECT_DOUBLE_EQ(1.0, band.GetScale(&success));
  band.SetScale(2.5);
  EXPECT_DOUBLE_EQ(2.5, band.GetScale(&success));

  band.SetDescription("My Band");
  EXPECT_STREQ("My Band", band.GetDescription());

  band.SetMetadataItem("HideNoDataValue", "YES");
  CPLStringList metadata;
  metadata.SetNameValue("FOO", "BAR");
  band.SetMetadata(metadata.List());
  EXPECT_STREQ("BAR", band.GetMetadataItem("FOO"));

  EXPECT_EQ(GCI_Undefined, band.GetColorInterpretation());
  band.SetColorInterpretation(GCI_RedBand);
  EXPECT_EQ(GCI_RedBand, band.GetColorInterpretation());

  CPLStringList cats;
  cats.AddString("CatA");
  cats.AddString("CatB");
  band.SetCategoryNames(cats.List());
  EXPECT_THAT(CslToVector(band.GetCategoryNames()),
              ::testing::ElementsAre("CatA", "CatB"));

  EXPECT_EQ(nullptr, band.GetColorTable());
  GDALColorTable ct;
  const GDALColorEntry ce = {10, 20, 30, 255};
  ct.SetColorEntry(0, &ce);
  band.SetColorTable(&ct);
  ASSERT_NE(nullptr, band.GetColorTable());
  EXPECT_EQ(GCI_PaletteIndex, band.GetColorInterpretation());
  band.SetColorTable(nullptr);
  EXPECT_EQ(nullptr, band.GetColorTable());

  EXPECT_EQ(nullptr, band.GetDefaultRAT());
  GDALDefaultRasterAttributeTable rat;
  band.SetDefaultRAT(&rat);
  EXPECT_NE(nullptr, band.GetDefaultRAT());
  band.SetDefaultRAT(nullptr);
  EXPECT_EQ(nullptr, band.GetDefaultRAT());
}

TEST_F(VrtRasterBandTest, NoDataValueVariants) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Float32);

  int success = 0;
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  band.SetNoDataValue(-9999.0);
  EXPECT_DOUBLE_EQ(-9999.0, band.GetNoDataValue(&success));
  EXPECT_TRUE(success);
  EXPECT_TRUE(band.IsNoDataValueInDataTypeRange());

  band.DeleteNoDataValue();
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  band.SetNoDataValue(0.0);
  band.UnsetNoDataValue();
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  band.SetNoDataValue(123.0);
  band.SetMetadataItem("HideNoDataValue", "1");
  band.GetNoDataValue(&success);
  EXPECT_FALSE(success);
  band.SetMetadataItem("HideNoDataValue", "0");
  band.GetNoDataValue(&success);
  EXPECT_TRUE(success);

  TestVRTRasterBand int64_band(&ds, 2, GDT_Int64);
  int64_band.SetNoDataValueAsInt64(-123456789012345LL);
  EXPECT_EQ(-123456789012345LL, int64_band.GetNoDataValueAsInt64(&success));
  EXPECT_TRUE(success);
  EXPECT_TRUE(int64_band.IsNoDataValueInDataTypeRange());

  TestVRTRasterBand uint64_band(&ds, 3, GDT_UInt64);
  uint64_band.SetNoDataValueAsUInt64(123456789012345ULL);
  EXPECT_EQ(123456789012345ULL, uint64_band.GetNoDataValueAsUInt64(&success));
  EXPECT_TRUE(success);
  EXPECT_TRUE(uint64_band.IsNoDataValueInDataTypeRange());

  // Calling Int64/UInt64 nodata accessors on mismatched band types must fail.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_INT64,
              band.GetNoDataValueAsInt64(&success));
    EXPECT_FALSE(success);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64,
              int64_band.GetNoDataValueAsUInt64(&success));
    EXPECT_FALSE(success);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_INT64,
              uint64_band.GetNoDataValueAsInt64(&success));
    EXPECT_FALSE(success);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtRasterBandTest, CopyCommonInfoFromSource) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);
  src_band->SetDescription("SourceBandDesc");
  EXPECT_EQ(CE_None, src_band->SetNoDataValue(255.0));
  EXPECT_EQ(CE_None, src_band->SetOffset(1.0));
  EXPECT_EQ(CE_None, src_band->SetScale(2.0));
  EXPECT_EQ(CE_None, src_band->SetUnitType("km"));
  EXPECT_EQ(CE_None, src_band->SetColorInterpretation(GCI_AlphaBand));
  src_band->SetMetadataItem("NBITS", "4", "IMAGE_STRUCTURE");
  src_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");

  GDALDefaultRasterAttributeTable rat;
  src_band->SetDefaultRAT(&rat);

  VRTDataset vrt_ds(10, 10);
  TestVRTRasterBand dst_band(&vrt_ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None, dst_band.CopyCommonInfoFrom(src_band));

  EXPECT_STREQ("SourceBandDesc", dst_band.GetDescription());
  int success = 0;
  EXPECT_DOUBLE_EQ(255.0, dst_band.GetNoDataValue(&success));
  EXPECT_TRUE(success);
  EXPECT_DOUBLE_EQ(1.0, dst_band.GetOffset());
  EXPECT_DOUBLE_EQ(2.0, dst_band.GetScale());
  EXPECT_STREQ("km", dst_band.GetUnitType());
  EXPECT_EQ(GCI_AlphaBand, dst_band.GetColorInterpretation());
  EXPECT_STREQ("4", dst_band.GetMetadataItem("NBITS", "IMAGE_STRUCTURE"));
  EXPECT_STREQ("SIGNEDBYTE",
               dst_band.GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"));
}

TEST_F(VrtRasterBandTest, Histograms) {
  VRTDataset ds(10, 10);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  GUIntBig hist[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(CE_None, band.SetDefaultHistogram(0.0, 10.0, 10, hist));

  double df_min = 0.0;
  double df_max = 0.0;
  int n_buckets = 0;
  GUIntBig* ret_hist_raw = nullptr;
  EXPECT_EQ(CE_None,
            band.GetDefaultHistogram(&df_min, &df_max, &n_buckets,
                                     &ret_hist_raw, FALSE, nullptr, nullptr));
  std::unique_ptr<GUIntBig, CplFreeDeleter> ret_hist(ret_hist_raw);
  EXPECT_DOUBLE_EQ(0.0, df_min);
  EXPECT_DOUBLE_EQ(10.0, df_max);
  EXPECT_EQ(10, n_buckets);
  ASSERT_NE(nullptr, ret_hist);
  EXPECT_EQ(1, ret_hist.get()[0]);
  EXPECT_EQ(10, ret_hist.get()[9]);

  GUIntBig hist2[10] = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  EXPECT_EQ(CE_None, band.SetDefaultHistogram(0.0, 10.0, 10, hist2));

  GUIntBig query_hist[10] = {0};
  EXPECT_EQ(CE_None, band.GetHistogram(0.0, 10.0, 10, query_hist, TRUE, TRUE,
                                       nullptr, nullptr));
  EXPECT_EQ(10, query_hist[0]);
}

TEST_F(VrtRasterBandTest, MaskBandOperations) {
  VRTDataset ds(10, 10);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  EXPECT_EQ(GMF_ALL_VALID, band.GetMaskFlags());
  EXPECT_NE(nullptr, band.GetMaskBand());

  EXPECT_EQ(CE_None, band.CreateMaskBand(0));
  EXPECT_EQ(0, band.GetMaskFlags());
  ASSERT_NE(nullptr, band.GetMaskBand());
  EXPECT_TRUE(band.GetMaskBand()->IsMaskBand());

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.CreateMaskBand(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  VRTDataset ds2(10, 10);
  TestVRTRasterBand band2(&ds2, 1, GDT_Byte);
  EXPECT_EQ(CE_None, band2.CreateMaskBand(GMF_PER_DATASET));
  EXPECT_EQ(GMF_PER_DATASET, band2.GetMaskFlags());
}

TEST_F(VrtRasterBandTest, XMLInitValidationAndOverviews) {
  VRTDataset ds(10, 10);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);
  VRTMapSharedResources map_resources;

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.XMLInit(nullptr, nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLXMLTreeCloser invalid_node(
        CPLCreateXMLNode(nullptr, CXT_Text, "Invalid"));
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band.XMLInit(invalid_node.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLXMLTreeCloser bad_type(
        CPLParseXMLString(R"xml(<VRTRasterBand dataType="InvalidType"/>)xml"));
    ASSERT_NE(nullptr, bad_type.get());
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.XMLInit(bad_type.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  constexpr char kOvrFile[] = "/vsimem/vrtrasterband_test/ovr_test.bin";
  VsiMemTempWrapper ovr_wrapper(kOvrFile, std::string(1, '\0'));

  constexpr char kFullXml[] = R"xml(
<VRTRasterBand dataType="Float32" band="1" blockXSize="64" blockYSize="64">
  <Description>FullDesc</Description>
  <NoDataValue>-999</NoDataValue>
  <HideNoDataValue>0</HideNoDataValue>
  <UnitType>m</UnitType>
  <Offset>10</Offset>
  <Scale>2</Scale>
  <ColorInterp>Gray</ColorInterp>
  <CategoryNames>
    <Category>A</Category>
  </CategoryNames>
  <ColorTable>
    <Entry c1="1" c2="2" c3="3" c4="255"/>
  </ColorTable>
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/ovr_test.bin</SourceFilename>
    <SourceBand>1</SourceBand>
  </Overview>
  <MaskBand>
    <VRTRasterBand dataType="Byte" subClass="VRTSourcedRasterBand"/>
  </MaskBand>
</VRTRasterBand>)xml";

  CPLXMLTreeCloser tree(CPLParseXMLString(kFullXml));
  ASSERT_NE(nullptr, tree.get());
  EXPECT_EQ(CE_None, band.XMLInit(tree.get(), nullptr, map_resources));

  EXPECT_EQ(GDT_Float32, band.GetRasterDataType());
  EXPECT_STREQ("FullDesc", band.GetDescription());
  EXPECT_STREQ("m", band.GetUnitType());
  EXPECT_DOUBLE_EQ(10.0, band.GetOffset());
  EXPECT_DOUBLE_EQ(2.0, band.GetScale());
  EXPECT_EQ(1, band.GetOverviewCount());
  EXPECT_EQ(0, band.GetMaskFlags());

  bool warned = false;
  size_t ram = 0;
  CPLXMLTreeCloser serialized(band.SerializeToXML(nullptr, warned, ram));
  ASSERT_NE(nullptr, serialized.get());
  std::unique_ptr<char, CplFreeDeleter> xml_str(
      CPLSerializeXMLTree(serialized.get()));
  ASSERT_NE(nullptr, xml_str);
  EXPECT_THAT(xml_str.get(), HasSubstr("FullDesc"));
  EXPECT_THAT(xml_str.get(), HasSubstr("Overview"));
  EXPECT_THAT(xml_str.get(), HasSubstr("MaskBand"));
  EXPECT_THAT(xml_str.get(), HasSubstr("CategoryNames"));
  EXPECT_THAT(xml_str.get(), HasSubstr("ColorTable"));

  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  absl::Cleanup cleanup_files = [&file_list] { CSLDestroy(file_list); };
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  absl::Cleanup cleanup_hash = [hash_set] { CPLHashSetDestroy(hash_set); };
  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_GE(file_list_size, 1);

  band.CloseDependentDatasets();
}

TEST_F(VrtRasterBandTest, OverviewAccessAndDetails) {
  constexpr char kOvrVrtPath[] = "/vsimem/vrtrasterband_test/ovr_target.vrt";
  constexpr char kOvrXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1"/>
</VRTDataset>)xml";
  VsiMemTempWrapper ovr_target(kOvrVrtPath, kOvrXml);

  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  constexpr char kBandXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/ovr_target.vrt</SourceFilename>
    <SourceBand>1</SourceBand>
  </Overview>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser tree(CPLParseXMLString(kBandXml));
  ASSERT_NE(nullptr, tree.get());
  VRTMapSharedResources map_resources;
  EXPECT_EQ(CE_None, band.XMLInit(tree.get(), nullptr, map_resources));

  EXPECT_EQ(1, band.GetOverviewCount());
  EXPECT_EQ(nullptr, band.GetOverview(-1));
  EXPECT_EQ(nullptr, band.GetOverview(5));
  GDALRasterBand* ovr_band = band.GetOverview(0);
  ASSERT_NE(nullptr, ovr_band);
  EXPECT_EQ(ovr_band, band.GetOverview(0));
  band.CloseDependentDatasets();

  // Recursive overview detection.
  ds.SetDescription("/vsimem/vrtrasterband_test/recursive.vrt");
  constexpr char kRecXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/recursive.vrt</SourceFilename>
    <SourceBand>1</SourceBand>
  </Overview>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser rec_tree(CPLParseXMLString(kRecXml));
  ASSERT_NE(nullptr, rec_tree.get());
  TestVRTRasterBand rec_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None, rec_band.XMLInit(rec_tree.get(), nullptr, map_resources));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, rec_band.GetOverview(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Non-existent overview file.
  constexpr char kBadFileXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/does_not_exist.tif</SourceFilename>
    <SourceBand>1</SourceBand>
  </Overview>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser bad_tree(CPLParseXMLString(kBadFileXml));
  ASSERT_NE(nullptr, bad_tree.get());
  TestVRTRasterBand bad_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None, bad_band.XMLInit(bad_tree.get(), nullptr, map_resources));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, bad_band.GetOverview(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Out-of-range source band on valid overview dataset.
  constexpr char kBadBandXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/ovr_target.vrt</SourceFilename>
    <SourceBand>99</SourceBand>
  </Overview>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser badband_tree(CPLParseXMLString(kBadBandXml));
  ASSERT_NE(nullptr, badband_tree.get());
  TestVRTRasterBand badband_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None,
            badband_band.XMLInit(badband_tree.get(), nullptr, map_resources));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, badband_band.GetOverview(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtRasterBandTest, VirtualOverviewsAndDisabledOverviews) {
  VRTDataset ds(40, 40);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  VRTRasterBand* band = cpl::down_cast<VRTRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, band);

  ds.SetEnableOverviews(false);
  EXPECT_EQ(0, band->GetOverviewCount());

  ds.SetEnableOverviews(true);
  {
    CPLConfigOptionSetter virtual_ovr("VRT_VIRTUAL_OVERVIEWS", "YES", false);
    int overviews[] = {2};
    EXPECT_EQ(CE_None, ds.BuildOverviews("NEAREST", 1, overviews, 0, nullptr,
                                         nullptr, nullptr));
  }

  EXPECT_EQ(1, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetOverview(-1));
  EXPECT_EQ(nullptr, band->GetOverview(10));
  EXPECT_NE(nullptr, band->GetOverview(0));
}

TEST_F(VrtRasterBandTest, XMLInitFailuresAndSubclasses) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  // Missing SourceFilename in Overview.
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview><SourceBand>1</SourceBand></Overview>
</VRTRasterBand>)xml"));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b1(&ds, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, b1.XMLInit(tree.get(), nullptr, map_resources));
    CPLErrorReset();
  }

  // MEM driver overview is rejected.
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename>MEM:::123</SourceFilename>
  </Overview>
</VRTRasterBand>)xml"));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b2(&ds, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, b2.XMLInit(tree.get(), kTestDir, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Relative path in SourceFilename succeeds during XMLInit (lazy open).
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename relativeToVRT="1">rel.tif</SourceFilename>
  </Overview>
</VRTRasterBand>)xml"));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b3(&ds, 1, GDT_Byte);
    EXPECT_EQ(CE_None,
              b3.XMLInit(tree.get(), "/vsimem/vrtrasterband_test/foo.vrt",
                         map_resources));
  }

  // Mismatching band attribute emits a warning but returns CE_None.
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(
        R"xml(<VRTRasterBand dataType="Byte" band="5"/>)xml"));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b4(&ds, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, b4.XMLInit(tree.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  constexpr char kDerivedMaskXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <MaskBand>
    <VRTRasterBand dataType="Byte" subClass="VRTDerivedRasterBand">
      <PixelFunctionType>near</PixelFunctionType>
    </VRTRasterBand>
  </MaskBand>
</VRTRasterBand>)xml";
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kDerivedMaskXml));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b5(&ds, 1, GDT_Byte);
    EXPECT_EQ(CE_None, b5.XMLInit(tree.get(), nullptr, map_resources));
  }

  constexpr char kRawMaskXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <MaskBand>
    <VRTRasterBand dataType="Byte" subClass="VRTRawRasterBand">
      <SourceFilename relativeToVRT="1">dummy.raw</SourceFilename>
    </VRTRasterBand>
  </MaskBand>
</VRTRasterBand>)xml";
  VsiMemTempWrapper raw_file("/vsimem/vrtrasterband_test/dummy.raw",
                             std::string(400, '\0'));

  // Raw raster band disabled at runtime: XMLInit fails even when file exists.
  {
    CPLConfigOptionSetter disable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO",
                                      false);
    CPLXMLTreeCloser tree(CPLParseXMLString(kRawMaskXml));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b6_disabled(&ds, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              b6_disabled.XMLInit(tree.get(), kTestDir, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Raw raster band enabled at runtime.
  {
    CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                     false);
    CPLXMLTreeCloser tree(CPLParseXMLString(kRawMaskXml));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b6(&ds, 1, GDT_Byte);
    EXPECT_EQ(CE_None, b6.XMLInit(tree.get(), kTestDir, map_resources));
  }

  {
    CPLXMLTreeCloser tree(CPLParseXMLString(R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <MaskBand>
    <VRTRasterBand dataType="Byte" subClass="VRTWarpedRasterBand"/>
  </MaskBand>
</VRTRasterBand>)xml"));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b7(&ds, 1, GDT_Byte);
    EXPECT_EQ(CE_None, b7.XMLInit(tree.get(), nullptr, map_resources));
  }

  {
    CPLXMLTreeCloser tree(CPLParseXMLString(R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <MaskBand>
    <VRTRasterBand dataType="Byte" subClass="UnknownSubClass"/>
  </MaskBand>
</VRTRasterBand>)xml"));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b8(&ds, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, b8.XMLInit(tree.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Band MaskBand fails when dataset mask band already exists.
  {
    VRTDataset ds_with_mask(20, 20);
    ASSERT_EQ(CE_None, ds_with_mask.CreateMaskBand(GMF_PER_DATASET));
    CPLXMLTreeCloser tree(CPLParseXMLString(kDerivedMaskXml));
    ASSERT_NE(nullptr, tree.get());
    TestVRTRasterBand b9(&ds_with_mask, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, b9.XMLInit(tree.get(), nullptr, map_resources));
    CPLErrorReset();
  }
}

TEST_F(VrtRasterBandTest, DatasetMaskBandInteractions) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1);
  EXPECT_EQ(CE_None, ds.CreateMaskBand(GMF_PER_DATASET));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.CreateMaskBand(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  EXPECT_NE(nullptr, band.GetMaskBand());
  EXPECT_EQ(GMF_PER_DATASET, band.GetMaskFlags());
}

TEST_F(VrtRasterBandTest, Int64AndUInt64NoDataAndSerialization) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  constexpr char kInt64Xml[] = R"xml(
<VRTRasterBand dataType="Int64" band="1">
  <NoDataValue>-9000000000000000000</NoDataValue>
  <HideNoDataValue>1</HideNoDataValue>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser tree64(CPLParseXMLString(kInt64Xml));
  ASSERT_NE(nullptr, tree64.get());
  TestVRTRasterBand int64_band(&ds, 1, GDT_Int64);
  EXPECT_EQ(CE_None, int64_band.XMLInit(tree64.get(), nullptr, map_resources));

  int success = 0;
  int64_band.GetNoDataValue(&success);
  EXPECT_FALSE(success);

  bool warned = false;
  size_t ram = 0;
  {
    CPLXMLTreeCloser serialized(
        int64_band.SerializeToXML(nullptr, warned, ram));
    ASSERT_NE(nullptr, serialized.get());
    std::unique_ptr<char, CplFreeDeleter> xml_str(
        CPLSerializeXMLTree(serialized.get()));
    ASSERT_NE(nullptr, xml_str);
    EXPECT_THAT(xml_str.get(), HasSubstr("-9000000000000000000"));
    EXPECT_THAT(xml_str.get(),
                HasSubstr("<HideNoDataValue>1</HideNoDataValue>"));
  }

  constexpr char kUInt64Xml[] = R"xml(
<VRTRasterBand dataType="UInt64" band="1">
  <NoDataValue>18000000000000000000</NoDataValue>
  <HideNoDataValue>0</HideNoDataValue>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser treeu64(CPLParseXMLString(kUInt64Xml));
  ASSERT_NE(nullptr, treeu64.get());
  TestVRTRasterBand uint64_band(&ds, 1, GDT_UInt64);
  EXPECT_EQ(CE_None,
            uint64_band.XMLInit(treeu64.get(), nullptr, map_resources));

  uint64_band.GetNoDataValue(&success);
  EXPECT_TRUE(success);

  {
    CPLXMLTreeCloser serialized(
        uint64_band.SerializeToXML(nullptr, warned, ram));
    ASSERT_NE(nullptr, serialized.get());
    std::unique_ptr<char, CplFreeDeleter> xml_str(
        CPLSerializeXMLTree(serialized.get()));
    ASSERT_NE(nullptr, xml_str);
    EXPECT_THAT(xml_str.get(), HasSubstr("18000000000000000000"));
  }

  TestVRTRasterBand f32_band(&ds, 1, GDT_Float32);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    f32_band.GetNoDataValueAsUInt64(&success);
    EXPECT_FALSE(success);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtRasterBandTest, HistogramsAndDefaultHistogram) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand band(&ds, 1, GDT_Byte);

  GUIntBig hist[10] = {0};
  EXPECT_EQ(CE_None, band.GetHistogram(0.0, 10.0, 10, hist, FALSE, FALSE,
                                       nullptr, nullptr));
  EXPECT_EQ(CE_None, band.SetDefaultHistogram(0.0, 10.0, 10, hist));

  double min_val = 0.0;
  double max_val = 0.0;
  int buckets = 0;
  GUIntBig* ret_hist_raw = nullptr;
  EXPECT_EQ(CE_None,
            band.GetDefaultHistogram(&min_val, &max_val, &buckets,
                                     &ret_hist_raw, FALSE, nullptr, nullptr));
  std::unique_ptr<GUIntBig, decltype([](void* p) { VSIFree(p); })> ret_hist(
      ret_hist_raw);
  EXPECT_EQ(10, buckets);

  bool warned = false;
  size_t ram = 0;
  CPLXMLTreeCloser serialized(band.SerializeToXML(nullptr, warned, ram));
  ASSERT_NE(nullptr, serialized.get());
  std::unique_ptr<char, CplFreeDeleter> xml_str(
      CPLSerializeXMLTree(serialized.get()));
  ASSERT_NE(nullptr, xml_str);
  EXPECT_THAT(xml_str.get(), HasSubstr("Histograms"));
}

TEST_F(VrtRasterBandTest, FileListWithNonExistentAndDuplicates) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  constexpr char kFile[] = "/vsimem/vrtrasterband_test/fl_test.bin";
  VsiMemTempWrapper file(kFile, "1");

  constexpr char kXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/fl_test.bin</SourceFilename>
  </Overview>
  <Overview>
    <SourceFilename relativeToVRT="0">/vsimem/vrtrasterband_test/does_not_exist.bin</SourceFilename>
  </Overview>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser tree(CPLParseXMLString(kXml));
  ASSERT_NE(nullptr, tree.get());
  TestVRTRasterBand band(&ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None, band.XMLInit(tree.get(), nullptr, map_resources));

  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  absl::Cleanup cleanup_files = [&file_list] { CSLDestroy(file_list); };
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  absl::Cleanup cleanup_hash = [hash_set] { CPLHashSetDestroy(hash_set); };

  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_EQ(1, file_list_size);

  // Calling again exercises the duplicate-entry skip branch in CPLHashSet.
  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_EQ(1, file_list_size);
}

TEST_F(VrtRasterBandTest, CategoryNamesAndColorTableExtraChildren) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  constexpr char kXml[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <CategoryNames>
    <Invalid>Skip</Invalid>
    <Category>RealCat</Category>
  </CategoryNames>
  <ColorTable>
    <Invalid>Skip</Invalid>
    <Entry c1="1" c2="2" c3="3" c4="255"/>
  </ColorTable>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser tree(CPLParseXMLString(kXml));
  ASSERT_NE(nullptr, tree.get());
  TestVRTRasterBand band(&ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None, band.XMLInit(tree.get(), nullptr, map_resources));

  char** cats = band.GetCategoryNames();
  ASSERT_NE(nullptr, cats);
  EXPECT_STREQ("RealCat", cats[0]);
  EXPECT_NE(nullptr, band.GetColorTable());
}

TEST_F(VrtRasterBandTest, NoDataRangeInfinityAndUnset) {
  VRTDataset ds(20, 20);
  TestVRTRasterBand float_band(&ds, 1, GDT_Float32);
  EXPECT_TRUE(float_band.IsNoDataValueInDataTypeRange());

  EXPECT_EQ(CE_None,
            float_band.SetNoDataValue(std::numeric_limits<double>::infinity()));
  EXPECT_TRUE(float_band.IsNoDataValueInDataTypeRange());

  TestVRTRasterBand byte_band(&ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None,
            byte_band.SetNoDataValue(std::numeric_limits<double>::infinity()));
  EXPECT_FALSE(byte_band.IsNoDataValueInDataTypeRange());
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and PAM fallback branches in
// VRTRasterBand::InitializeFromVRT in vrtrasterband.cpp.
