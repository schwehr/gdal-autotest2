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

#define protected public
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#undef protected

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

class DummyBand : public GDALRasterBand {
 public:
  DummyBand(GDALDataset* ds, int band_num, int xsize, int ysize) {
    poDS = ds;
    nBand = band_num;
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    eDataType = GDT_Byte;
    eAccess = GA_Update;
    nBlockXSize = xsize;
    nBlockYSize = 1;
    data_.resize(static_cast<size_t>(xsize) * ysize, 7);
  }

  CPLErr IReadBlock(int, int nBlockYOff, void* pImage) override {
    std::memcpy(pImage, data_.data() + nBlockYOff * nRasterXSize, nRasterXSize);
    return CE_None;
  }

  CPLErr IWriteBlock(int, int nBlockYOff, void* pImage) override {
    std::memcpy(data_.data() + nBlockYOff * nRasterXSize, pImage, nRasterXSize);
    return CE_None;
  }

  CPLErr CreateMaskBand(int nFlags) override {
    mask_band_ =
        std::make_unique<DummyBand>(poDS, 1, nRasterXSize, nRasterYSize);
    mask_flags_ = nFlags;
    return CE_None;
  }

  GDALRasterBand* GetMaskBand() override {
    if (mask_band_) return mask_band_.get();
    return GDALRasterBand::GetMaskBand();
  }

  int GetMaskFlags() override {
    if (mask_band_) return mask_flags_;
    return GDALRasterBand::GetMaskFlags();
  }

  CPLErr SetDescriptionCounter(const char* desc) {
    SetDescription(desc);
    return CE_None;
  }

  CPLErr SetColorTable(GDALColorTable* ct) override {
    if (ct) {
      ct_.reset(ct->Clone());
    } else {
      ct_.reset();
    }
    return CE_None;
  }
  GDALColorTable* GetColorTable() override { return ct_.get(); }

  CPLErr SetColorInterpretation(GDALColorInterp ci) override {
    ci_ = ci;
    return CE_None;
  }
  GDALColorInterp GetColorInterpretation() override { return ci_; }

  CPLErr SetNoDataValue(double val) override {
    has_ndv_ = true;
    ndv_ = val;
    return CE_None;
  }
  double GetNoDataValue(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_ndv_ ? 1 : 0;
    return ndv_;
  }

  CPLErr SetOffset(double val) override {
    has_offset_ = true;
    offset_ = val;
    return CE_None;
  }
  double GetOffset(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_offset_ ? 1 : 0;
    return offset_;
  }

  CPLErr SetScale(double val) override {
    has_scale_ = true;
    scale_ = val;
    return CE_None;
  }
  double GetScale(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_scale_ ? 1 : 0;
    return scale_;
  }

  CPLErr SetUnitType(const char* unit) override {
    unit_ = unit ? unit : "";
    return CE_None;
  }
  const char* GetUnitType() override { return unit_.c_str(); }

  CPLErr SetCategoryNames(char** names) override {
    cats_.Assign(CSLDuplicate(names), true);
    return CE_None;
  }
  char** GetCategoryNames() override { return cats_.List(); }

  CPLErr SetDefaultRAT(const GDALRasterAttributeTable* rat) override {
    if (rat) {
      rat_.reset(rat->Clone());
    } else {
      rat_.reset();
    }
    return CE_None;
  }
  GDALRasterAttributeTable* GetDefaultRAT() override { return rat_.get(); }

 private:
  std::vector<GByte> data_;
  std::unique_ptr<DummyBand> mask_band_;
  int mask_flags_ = GMF_ALL_VALID;
  std::unique_ptr<GDALColorTable> ct_;
  GDALColorInterp ci_ = GCI_Undefined;
  bool has_ndv_ = false;
  double ndv_ = 0.0;
  bool has_offset_ = false;
  double offset_ = 0.0;
  bool has_scale_ = false;
  double scale_ = 1.0;
  std::string unit_;
  CPLStringList cats_;
  std::unique_ptr<GDALRasterAttributeTable> rat_;
};

class DummyDataset : public GDALDataset {
 public:
  DummyDataset(int xsize, int ysize, int bands) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    eAccess = GA_Update;
    for (int i = 1; i <= bands; ++i) {
      SetBand(i, new DummyBand(this, i, xsize, ysize));
    }
  }

  void SetAccess(GDALAccess eAccessIn) { eAccess = eAccessIn; }

  CPLErr SetGeoTransform(const GDALGeoTransform& gt) override {
    gt_ = gt;
    has_gt_ = true;
    return CE_None;
  }
  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    if (!has_gt_) return CE_Failure;
    gt = gt_;
    return CE_None;
  }

  CPLErr SetSpatialRef(const OGRSpatialReference* srs) override {
    if (srs) {
      srs_ = *srs;
    } else {
      srs_.Clear();
    }
    return CE_None;
  }
  const OGRSpatialReference* GetSpatialRef() const override {
    return srs_.IsEmpty() ? nullptr : &srs_;
  }

  CPLErr SetGCPs(int nCount, const GDAL_GCP* pasGCPList,
                 const OGRSpatialReference* poGCP_SRS) override {
    if (gcp_count_ > 0) {
      GDALDeinitGCPs(gcp_count_, gcps_);
      CPLFree(gcps_);
    }
    gcp_count_ = nCount;
    gcps_ = GDALDuplicateGCPs(nCount, pasGCPList);
    if (poGCP_SRS) gcp_srs_ = *poGCP_SRS;
    return CE_None;
  }
  int GetGCPCount() override { return gcp_count_; }
  const GDAL_GCP* GetGCPs() override { return gcps_; }
  const OGRSpatialReference* GetGCPSpatialRef() const override {
    return gcp_srs_.IsEmpty() ? nullptr : &gcp_srs_;
  }

  ~DummyDataset() override {
    if (gcp_count_ > 0) {
      GDALDeinitGCPs(gcp_count_, gcps_);
      CPLFree(gcps_);
    }
  }

  CPLErr CreateMaskBand(int nFlags) override {
    for (int i = 1; i <= nBands; ++i) {
      GetRasterBand(i)->CreateMaskBand(nFlags);
    }
    return CE_None;
  }

  char** GetFileList() override {
    if (file_list_.empty()) return GDALDataset::GetFileList();
    return CSLDuplicate(file_list_.List());
  }

  std::shared_ptr<GDALGroup> GetRootGroup() const override {
    return root_group_;
  }

  int GetLayerCount() const override {
    return static_cast<int>(layers_.size());
  }
  const OGRLayer* GetLayer(int i) const override {
    if (i < 0 || i >= static_cast<int>(layers_.size())) return nullptr;
    return layers_[i];
  }
  int TestCapability(const char* cap) const override {
    if (EQUAL(cap, ODsCCreateLayer)) return can_create_layer_ ? 1 : 0;
    return 0;
  }
  OGRLayer* CopyLayer(OGRLayer* src_layer, const char*, char**) override {
    copied_layers_++;
    return src_layer;
  }

  CPLStringList file_list_;
  std::shared_ptr<GDALGroup> root_group_;
  std::vector<OGRLayer*> layers_;
  bool can_create_layer_ = false;
  int copied_layers_ = 0;

 private:
  GDALGeoTransform gt_;
  bool has_gt_ = false;
  OGRSpatialReference srs_;
  int gcp_count_ = 0;
  GDAL_GCP* gcps_ = nullptr;
  OGRSpatialReference gcp_srs_;
};

class DummyGroup : public GDALGroup {
 public:
  DummyGroup() : GDALGroup("", "/") {}
  std::vector<std::string> GetMDArrayNames(
      CSLConstList = nullptr) const override {
    return {};
  }
  std::shared_ptr<GDALMDArray> OpenMDArray(
      const std::string&, CSLConstList = nullptr) const override {
    return nullptr;
  }
  std::vector<std::string> GetGroupNames(
      CSLConstList = nullptr) const override {
    return {};
  }
  std::shared_ptr<GDALGroup> OpenGroup(const std::string&,
                                       CSLConstList = nullptr) const override {
    return nullptr;
  }
  bool CopyFrom(const std::shared_ptr<GDALGroup>&, GDALDataset*,
                const std::shared_ptr<GDALGroup>&, bool, GUInt64&,
                const GUInt64, GDALProgressFunc, void*, CSLConstList) override {
    return !fail_copy_;
  }
  bool fail_copy_ = false;
};

class TestAlgo : public GDALAlgorithm {
 public:
  TestAlgo() : GDALAlgorithm("test-op", "Test operation", "/help") {}
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

TEST(GdalDriverTest, MetadataAndInfoGettersAndExtensionSync) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(nullptr, GDALGetDriverShortName(nullptr));
  EXPECT_EQ(nullptr, GDALGetDriverLongName(nullptr));
  EXPECT_EQ(nullptr, GDALGetDriverHelpTopic(nullptr));
  EXPECT_EQ(nullptr, GDALGetDriverCreationOptionList(nullptr));

  auto driver = std::make_unique<GDALDriver>();
  driver->SetDescription("MYDRV");
  driver->SetMetadataItem(GDAL_DMD_LONGNAME, "My Long Driver Name");
  driver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/mydrv.html");

  GDALDriverH hDrv = GDALDriver::ToHandle(driver.get());
  EXPECT_EQ(driver.get(), GDALDriver::FromHandle(hDrv));
  EXPECT_STREQ("MYDRV", GDALGetDriverShortName(hDrv));
  EXPECT_STREQ("My Long Driver Name", GDALGetDriverLongName(hDrv));
  EXPECT_STREQ("drivers/raster/mydrv.html", GDALGetDriverHelpTopic(hDrv));
  EXPECT_STREQ("", GDALGetDriverCreationOptionList(hDrv));

  // Extension <-> Extensions auto-sync in SetMetadataItem.
  driver->SetMetadataItem(GDAL_DMD_EXTENSION, "abc");
  EXPECT_STREQ("abc", driver->GetMetadataItem(GDAL_DMD_EXTENSIONS));

  auto driver2 = std::make_unique<GDALDriver>();
  driver2->SetMetadataItem(GDAL_DMD_EXTENSIONS, "xyz");
  EXPECT_STREQ("xyz", driver2->GetMetadataItem(GDAL_DMD_EXTENSION));

  // Overview creation option list fallback to GTiff for raster drivers.
  driver2->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  const char* ovr_xml =
      driver2->GetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST);
  if (GetGDALDriverManager()->GetDriverByName("GTiff")) {
    EXPECT_NE(nullptr, ovr_xml);
  }
  driver2->SetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST, "<Custom/>");
  EXPECT_STREQ("<Custom/>",
               driver2->GetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST));

  GDALDestroyDriver(GDALDriver::ToHandle(driver.release()));
  CPLPopErrorHandler();
}

TEST(GdalDriverTest, ValidateOptionsAllTypesAndBranches) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDriver drv;
  drv.SetDescription("OPTDRV");

  const char* kOptionXML =
      "<CreationOptionList>"
      "  <Option name='INT_OPT' type='INT' min='-10' max='100' "
      "alias='INT_ALIAS' "
      "alt_config_option='MY_CFG_OPT' description='d' default='0' "
      "required='no' scope='raster'/>"
      "  <Option name='INT_DEP' type='INTEGER' deprecated_alias='INT_OLD'/>"
      "  <Option name='UINT_OPT' type='UNSIGNED INT' min='1' max='50'/>"
      "  <Option name='FLOAT_OPT' type='FLOAT' min='-1.5' max='10.5'/>"
      "  <Option name='BOOL_OPT' type='BOOLEAN'/>"
      "  <Option name='SEL_OPT' type='STRING-SELECT'>"
      "    <Value alias='VAL_A' deprecated_alias='OLD_A'>A</Value>"
      "    <Value>B</Value>"
      "  </Option>"
      "  <Option name='STR_OPT' type='STRING' maxsize='4'/>"
      "  <Option name='PREFIX_*' type='STRING'/>"
      "  <Option name='*_SUFFIX' type='STRING'/>"
      "  <Option name='MID_*_END' type='STRING'/>"
      "  <Option name='BADTYPE_OPT' type='WEIRD' unknown_attr='foo'/>"
      "  <Option name='NOTYPE_OPT'/>"
      "</CreationOptionList>";

  drv.SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, kOptionXML);
  drv.SetMetadataItem(
      GDAL_DMD_OPENOPTIONLIST,
      "<OpenOptionList>"
      "  <Option name='OPEN_A' type='STRING' alias='OPEN_ALIAS'/>"
      "</OpenOptionList>");

  EXPECT_TRUE(drv.HasOpenOption("OPEN_A"));
  EXPECT_TRUE(GDALDriverHasOpenOption(GDALDriver::ToHandle(&drv), "OPEN_A"));
  EXPECT_FALSE(GDALDriverHasOpenOption(nullptr, "OPEN_A"));
  EXPECT_FALSE(drv.HasOpenOption(nullptr));
  EXPECT_FALSE(drv.HasOpenOption("OPEN_ALIAS"));
  EXPECT_FALSE(drv.HasOpenOption("NO_SUCH_OPT"));

  GDALDriverH hDrv = GDALDriver::ToHandle(&drv);
  EXPECT_TRUE(GDALValidateCreationOptions(hDrv, nullptr));
  const char* const open_opts_val[] = {"OPEN_A=ok", "UNKNOWN_OPEN=1",
                                       "VALIDATE_OPEN_OPTIONS=NO", nullptr};
  EXPECT_TRUE(GDALValidateOpenOptions(hDrv, open_opts_val));

  // Valid options (including wildcards PREFIX_*, *_SUFFIX, MID*_END).
  const char* const valid_opts[] = {
      "INT_OPT=+42",    "INT_ALIAS=-5",   "INT_OLD=10",
      "UINT_OPT=25",    "FLOAT_OPT=3.25", "BOOL_OPT=YES",
      "BOOL_OPT=OFF",   "SEL_OPT=A",      "SEL_OPT=VAL_A",
      "SEL_OPT=OLD_A",  "STR_OPT=abcd",   "PREFIX_FOO=bar",
      "FOO_SUFFIX=bar", "MID_X_END=bar",  "APPEND_SUBDATASET=YES",
      "@silent_opt=1",  nullptr};
  EXPECT_TRUE(GDALValidateCreationOptions(hDrv, valid_opts));

  // Invalid options for each type and range check.
  const char* const invalid_opts[] = {"NO_EQUAL_SIGN",
                                      "UNKNOWN_OPT=1",
                                      "INT_OPT=abc",
                                      "INT_OPT=-20",
                                      "INT_OPT=200",
                                      "UINT_OPT=-1",
                                      "UINT_OPT=0",
                                      "UINT_OPT=100",
                                      "FLOAT_OPT=not_a_float",
                                      "FLOAT_OPT=-5.0",
                                      "FLOAT_OPT=20.0",
                                      "BOOL_OPT=MAYBE",
                                      "SEL_OPT=C",
                                      "STR_OPT=toolong",
                                      "BADTYPE_OPT=1",
                                      "NOTYPE_OPT=1",
                                      nullptr};
  EXPECT_FALSE(GDALValidateCreationOptions(hDrv, invalid_opts));

  // Invalid XML in CreationOptionList.
  drv.SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, "<BrokenXML");
  const char* const one_opt[] = {"INT_OPT=1", nullptr};
  EXPECT_TRUE(GDALValidateCreationOptions(hDrv, one_opt));
  CPLPopErrorHandler();
}

TEST(GdalDriverTest, CreateAndCreateMultiDimensionalAndOpen) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDriver drv;
  drv.SetDescription("TESTDRV");

  // 1. Unsupported Create when no callbacks are set.
  EXPECT_EQ(nullptr,
            drv.Create("/vsimem/drv_test1.bin", 10, 10, 1, GDT_Byte, nullptr));
  // Invalid dimensions/types (when GDAL_DCAP_RASTER is set and not VECTOR).
  drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  drv.pfnCreate = [](const char*, int x, int y, int b, GDALDataType,
                     char**) -> GDALDataset* {
    return new DummyDataset(x, y, b);
  };
  EXPECT_EQ(nullptr,
            drv.Create("/vsimem/drv_test1.bin", 0, 10, 1, GDT_Byte, nullptr));
  EXPECT_EQ(nullptr,
            drv.Create("/vsimem/drv_test1.bin", 10, -1, 1, GDT_Byte, nullptr));
  EXPECT_EQ(nullptr,
            drv.Create("/vsimem/drv_test1.bin", 10, 10, -1, GDT_Byte, nullptr));
  EXPECT_EQ(nullptr, drv.Create("/vsimem/drv_test1.bin", 10, 10, 1, GDT_Unknown,
                                nullptr));

  // Valid pfnCreate.
  std::unique_ptr<GDALDataset> ds1(
      drv.Create("/vsimem/drv_test1.bin", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds1);
  EXPECT_EQ(&drv, ds1->GetDriver());
  EXPECT_EQ(GA_Update, ds1->GetAccess());

  // pfnCreateEx path.
  drv.pfnCreate = nullptr;
  drv.pfnCreateEx = [](GDALDriver*, const char*, int x, int y, int b,
                       GDALDataType, char**) -> GDALDataset* {
    return new DummyDataset(x, y, b);
  };
  std::unique_ptr<GDALDataset> ds2(
      drv.Create("/vsimem/drv_test2.bin", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds2);

  // pfnCreateVectorOnly path.
  drv.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  drv.pfnCreateEx = nullptr;
  drv.pfnCreateVectorOnly = [](GDALDriver*, const char*,
                               char**) -> GDALDataset* {
    return new DummyDataset(0, 0, 0);
  };
  std::unique_ptr<GDALDataset> ds3(GDALDataset::FromHandle(
      GDALCreate(GDALDriver::ToHandle(&drv), "/vsimem/drv_vec.bin", 0, 0, 0,
                 GDT_Unknown, nullptr)));
  ASSERT_NE(nullptr, ds3);

  // CreateMultiDimensional
  EXPECT_EQ(nullptr,
            drv.CreateMultiDimensional("/vsimem/mdim.bin", nullptr, nullptr));
  drv.pfnCreateMultiDimensional = [](const char*, CSLConstList,
                                     CSLConstList) -> GDALDataset* {
    auto* ds = new DummyDataset(0, 0, 0);
    ds->root_group_ = std::make_shared<DummyGroup>();
    return ds;
  };
  drv.SetMetadataItem(GDAL_DMD_MULTIDIM_GROUP_CREATIONOPTIONLIST,
                      "<MultiDimGroupCreationOptionList>"
                      "  <Option name='GRP_OPT' type='STRING'/>"
                      "</MultiDimGroupCreationOptionList>");
  const char* const rg_opts[] = {"GRP_OPT=val", nullptr};
  std::unique_ptr<GDALDataset> mdim_ds(
      GDALDataset::FromHandle(GDALCreateMultiDimensional(
          GDALDriver::ToHandle(&drv), "/vsimem/mdim.bin", rg_opts, nullptr)));
  ASSERT_NE(nullptr, mdim_ds);

  // Open via pfnOpen and pfnOpenWithDriverArg.
  GDALOpenInfo open_info("/vsimem/drv_test1.bin", GA_ReadOnly);
  const char* const open_opts[] = {"OPEN_OPT=1", nullptr};
  open_info.papszOpenOptions = const_cast<char**>(open_opts);
  EXPECT_EQ(nullptr, drv.Open(&open_info, true));

  drv.pfnOpen = [](GDALOpenInfo*) -> GDALDataset* {
    return new DummyDataset(2, 2, 1);
  };
  std::unique_ptr<GDALDataset> opened1(drv.Open(&open_info, true));
  ASSERT_NE(nullptr, opened1);

  drv.pfnOpen = nullptr;
  drv.pfnOpenWithDriverArg = [](GDALDriver*, GDALOpenInfo*) -> GDALDataset* {
    return new DummyDataset(2, 2, 1);
  };
  std::unique_ptr<GDALDataset> opened2(drv.Open(&open_info, false));
  ASSERT_NE(nullptr, opened2);
  CPLPopErrorHandler();
}

TEST(GdalDriverTest, DefaultCreateCopyAndMetadataAndMasks) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDriver drv;
  drv.SetDescription("COPYDRV");
  drv.pfnCreate = [](const char*, int x, int y, int b, GDALDataType,
                     char**) -> GDALDataset* {
    return new DummyDataset(x, y, b);
  };

  // Source dataset with rich metadata, GCPs, GeoTransform, SRS, and band props.
  DummyDataset src(4, 4, 2);
  src.SetMetadataItem("KEY1", "VAL1");
  src.SetMetadataItem("SUB1", "S1", "SUBDATASETS");
  src.SetMetadataItem("CUSTOM", "C1", "MY_DOMAIN");
  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  src.SetGeoTransform(gt);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  src.SetSpatialRef(&srs);

  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  gcp.dfGCPPixel = 1.0;
  gcp.dfGCPLine = 2.0;
  gcp.dfGCPX = 11.0;
  gcp.dfGCPY = 18.0;
  src.SetGCPs(1, &gcp, &srs);
  GDALDeinitGCPs(1, &gcp);

  auto* b1 = static_cast<DummyBand*>(src.GetRasterBand(1));
  b1->SetDescription("Band1Desc");
  GDALColorTable ct;
  GDALColorEntry ce = {255, 0, 0, 255};
  ct.SetColorEntry(0, &ce);
  b1->SetColorTable(&ct);
  b1->SetColorInterpretation(GCI_PaletteIndex);
  b1->SetNoDataValue(-999.0);
  b1->SetOffset(1.5);
  b1->SetScale(2.5);
  b1->SetUnitType("meters");
  const char* const cats[] = {"CatA", "CatB", nullptr};
  b1->SetCategoryNames(const_cast<char**>(cats));
  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Value", GFT_Integer, GFU_MinMax);
  rat.SetValue(0, 0, 1);
  b1->SetDefaultRAT(&rat);
  b1->SetMetadataItem("BKEY", "BVAL");
  b1->SetMetadataItem("BCUSTOM", "BCVAL", "BAND_DOM");
  b1->CreateMaskBand(GMF_PER_DATASET);

  // CreateCopy using DefaultCreateCopy (since pfnCreateCopy is nullptr).
  const char* const copy_mdd_opts[] = {"COPY_SRC_MDD=YES", nullptr};
  std::unique_ptr<GDALDataset> dst(GDALDataset::FromHandle(GDALCreateCopy(
      GDALDriver::ToHandle(&drv), "/vsimem/copy_dst.bin",
      GDALDataset::ToHandle(&src), FALSE, copy_mdd_opts, nullptr, nullptr)));
  ASSERT_NE(nullptr, dst);
  EXPECT_STREQ("VAL1", dst->GetMetadataItem("KEY1"));
  EXPECT_STREQ("C1", dst->GetMetadataItem("CUSTOM", "MY_DOMAIN"));
  EXPECT_STREQ("Band1Desc", dst->GetRasterBand(1)->GetDescription());
  EXPECT_NE(nullptr, dst->GetRasterBand(1)->GetColorTable());
  EXPECT_EQ(GCI_PaletteIndex, dst->GetRasterBand(1)->GetColorInterpretation());
  int has_ndv = 0;
  EXPECT_DOUBLE_EQ(-999.0, dst->GetRasterBand(1)->GetNoDataValue(&has_ndv));
  EXPECT_EQ(1, has_ndv);
  EXPECT_DOUBLE_EQ(1.5, dst->GetRasterBand(1)->GetOffset(nullptr));
  EXPECT_DOUBLE_EQ(2.5, dst->GetRasterBand(1)->GetScale(nullptr));
  EXPECT_NE(nullptr, dst->GetRasterBand(1)->GetCategoryNames());
  EXPECT_NE(nullptr, dst->GetRasterBand(1)->GetDefaultRAT());

  // Also test DefaultCopyMasks 3-arg overload with per-band mask
  // (non-PER_DATASET).
  b1->CreateMaskBand(0);
  EXPECT_EQ(CE_None, GDALDriver::DefaultCopyMasks(&src, dst.get(), FALSE));

  // Test DefaultCreateCopy with 0-band multidimensional dataset.
  DummyDataset mdim_src(0, 0, 0);
  mdim_src.root_group_ = std::make_shared<DummyGroup>();
  drv.SetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER, "YES");
  drv.pfnCreateMultiDimensional = [](const char*, CSLConstList,
                                     CSLConstList) -> GDALDataset* {
    auto* ds = new DummyDataset(0, 0, 0);
    ds->root_group_ = std::make_shared<DummyGroup>();
    return ds;
  };
  std::unique_ptr<GDALDataset> mdim_dst(drv.CreateCopy(
      "/vsimem/mdim_copy.bin", &mdim_src, FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, mdim_dst);

  // Test pfnCreateCopy custom callback and APPEND_SUBDATASET=YES.
  drv.pfnCreateCopy = [](const char*, GDALDataset*, int, char**,
                         GDALProgressFunc, void*) -> GDALDataset* {
    return new DummyDataset(2, 2, 1);
  };
  const char* const append_opts[] = {"APPEND_SUBDATASET=YES",
                                     "_INTERNAL_DATASET=YES", nullptr};
  std::unique_ptr<GDALDataset> copy2(drv.CreateCopy(
      "/vsimem/copy2.bin", &src, FALSE, append_opts, nullptr, nullptr));
  ASSERT_NE(nullptr, copy2);

  // Additional DefaultCreateCopy branches:
  // 1) Automatic INTERLEAVE propagation + NBITS/PIXELTYPE propagation +
  //    @QUIET_DELETE_ON_CREATE_COPY=NO:
  drv.pfnCreateCopy = nullptr;
  drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  drv.SetMetadataItem(
      GDAL_DMD_CREATIONOPTIONLIST,
      "<CreationOptionList>"
      "  <Option name='INTERLEAVE' type='STRING-SELECT'>"
      "    <Value>BAND</Value><Value>LINE</Value><Value>PIXEL</Value>"
      "  </Option>"
      "  <Option name='NBITS' type='INT'/>"
      "  <Option name='PIXELTYPE' type='STRING'/>"
      "</CreationOptionList>");
  src.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  b1->SetMetadataItem("NBITS", "4", "IMAGE_STRUCTURE");
  b1->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  const char* const qd_opts[] = {"@QUIET_DELETE_ON_CREATE_COPY=NO",
                                 "SKIP_HOLES=YES", nullptr};
  std::unique_ptr<GDALDataset> copy3(drv.CreateCopy(
      "/vsimem/copy3.bin", &src, FALSE, qd_opts, nullptr, nullptr));
  ASSERT_NE(nullptr, copy3);

  // 2) Progress callback returning 0 at start of DefaultCreateCopy:
  EXPECT_EQ(nullptr,
            drv.CreateCopy(
                "/vsimem/copy_cancel.bin", &src, FALSE, nullptr,
                [](double, const char*, void*) -> int { return 0; }, nullptr));

  // 3) 0-band non-vector dataset failure in DefaultCreateCopy:
  DummyDataset zero_ds(4, 4, 0);
  EXPECT_EQ(nullptr, drv.CreateCopy("/vsimem/copy_zero.bin", &zero_ds, FALSE,
                                    nullptr, nullptr, nullptr));

  // 4) Raster-only vs Vector-only driver mismatch in DefaultCreateCopy:
  GDALDriver ras_only_drv;
  ras_only_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  GDALDriver vec_only_drv;
  vec_only_drv.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  src.poDriver = &ras_only_drv;
  EXPECT_EQ(nullptr,
            vec_only_drv.DefaultCreateCopy("/vsimem/mismatch1", &src, FALSE,
                                           nullptr, nullptr, nullptr));
  src.poDriver = &vec_only_drv;
  EXPECT_EQ(nullptr,
            ras_only_drv.DefaultCreateCopy("/vsimem/mismatch2", &src, FALSE,
                                           nullptr, nullptr, nullptr));
  src.poDriver = nullptr;

  // 5) Output driver creating wrong number of bands (nDstBands != nBands):
  GDALDriver bad_bands_drv;
  bad_bands_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  bad_bands_drv.pfnCreate = [](const char*, int x, int y, int, GDALDataType,
                               char**) -> GDALDataset* {
    return new DummyDataset(x, y, 1);  // returns 1 band when 2 requested!
  };
  EXPECT_EQ(nullptr,
            bad_bands_drv.DefaultCreateCopy("/vsimem/bad_bands.bin", &src,
                                            FALSE, nullptr, nullptr, nullptr));

  // 6) Vector layers copy in DefaultCreateCopy when nDstBands == 0:
  GDALDriver vec_copy_drv;
  vec_copy_drv.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  vec_copy_drv.pfnCreate = [](const char*, int, int, int, GDALDataType,
                              char**) -> GDALDataset* {
    auto* d = new DummyDataset(0, 0, 0);
    d->can_create_layer_ = true;
    return d;
  };
  GDALDriver* mem_vec_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_vec_drv) {
    std::unique_ptr<GDALDataset> mem_vds(
        mem_vec_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    if (mem_vds) {
      OGRLayer* lyr = mem_vds->CreateLayer("lyr1", nullptr, wkbPoint, nullptr);
      DummyDataset vsrc(0, 0, 0);
      vsrc.SetGeoTransform(gt);
      vsrc.SetSpatialRef(&srs);
      vsrc.layers_.push_back(lyr);
      vsrc.layers_.push_back(nullptr);
      std::unique_ptr<GDALDataset> vdst(vec_copy_drv.DefaultCreateCopy(
          "/vsimem/vec_copy.bin", &vsrc, FALSE, nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, vdst);
      EXPECT_EQ(1, static_cast<DummyDataset*>(vdst.get())->copied_layers_);
    }
  }

  // 7) QuietDeleteForCreateCopy when source and dest share a file vs don't
  // share:
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (gtiff_drv) {
    std::unique_ptr<GDALDataset> t1(
        gtiff_drv->Create("/vsimem/qd_share.tif", 4, 4, 1, GDT_Byte, nullptr));
    t1->SetMetadataItem("K", "V");  // creates .aux.xml on close/flush
    t1.reset();
    VSIFCloseL(VSIFOpenL("/vsimem/qd_share.tif.aux.xml", "wb"));
    std::unique_ptr<GDALDataset> t1_open(
        GDALDataset::Open("/vsimem/qd_share.tif", GDAL_OF_RASTER));
    ASSERT_NE(nullptr, t1_open);
    // Overwrite /vsimem/qd_share.tif using t1_open as source!
    std::unique_ptr<GDALDataset> t2(
        gtiff_drv->CreateCopy("/vsimem/qd_share.tif", t1_open.get(), FALSE,
                              nullptr, nullptr, nullptr));
    t2.reset();
    t1_open.reset();
    VSIUnlink("/vsimem/qd_share.tif");
    VSIUnlink("/vsimem/qd_share.tif.aux.xml");
  }

  CPLPopErrorHandler();
}

TEST(GdalDriverTest, DeleteRenameCopyFilesAndVectorTranslate) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const char* f1 = "/vsimem/drv_op_1.dat";
  const char* f1_aux = "/vsimem/drv_op_1.dat.aux.xml";
  const char* f2 = "/vsimem/drv_op_2.dat";
  const char* f3 = "/vsimem/drv_op_3.dat";

  VSIFCloseL(VSIFOpenL(f1, "wb"));
  VSIFCloseL(VSIFOpenL(f1_aux, "wb"));

  GDALDriver drv;
  drv.SetDescription("FILEDRV");
  drv.pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
    VSIStatBufL sStat;
    if (VSIStatL(poOpenInfo->pszFilename, &sStat) != 0) return nullptr;
    auto* ds = new DummyDataset(2, 2, 1);
    ds->SetDescription(poOpenInfo->pszFilename);
    ds->file_list_.AddString(poOpenInfo->pszFilename);
    std::string aux = std::string(poOpenInfo->pszFilename) + ".aux.xml";
    if (VSIStatL(aux.c_str(), &sStat) == 0) {
      ds->file_list_.AddString(aux.c_str());
    }
    return ds;
  };
  drv.pfnIdentify = [](GDALOpenInfo* poOpenInfo) -> int {
    return strstr(poOpenInfo->pszFilename, "drv_op_") != nullptr ? 1 : 0;
  };

  GetGDALDriverManager()->RegisterDriver(&drv);
  GDALDriverH hDrv = GDALDriver::ToHandle(&drv);

  // CopyDatasetFiles (using DefaultCopyFiles).
  EXPECT_EQ(CE_None, GDALCopyDatasetFiles(hDrv, f2, f1));
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(f2, &sStat));
  EXPECT_EQ(0, VSIStatL("/vsimem/drv_op_2.dat.aux.xml", &sStat));

  // RenameDataset (using DefaultRename).
  EXPECT_EQ(CE_None, GDALRenameDataset(hDrv, f3, f2));
  EXPECT_EQ(0, VSIStatL(f3, &sStat));
  EXPECT_NE(0, VSIStatL(f2, &sStat));

  // DeleteDataset via GetFileList() when pfnDelete is nullptr.
  EXPECT_EQ(CE_None, GDALDeleteDataset(hDrv, f3));
  EXPECT_NE(0, VSIStatL(f3, &sStat));

  // QuietDelete with allowed drivers on nonexistent f2 (returns CE_Failure
  // since pfnIdentify matches "drv_op_" and Delete fails to open f2) and on
  // existing f1 (returns CE_None and unlinks f1).
  const char* const allowed_filedrv[] = {"FILEDRV", nullptr};
  EXPECT_EQ(CE_Failure, GDALDriver::QuietDelete(f2, allowed_filedrv));
  EXPECT_EQ(CE_None, GDALDriver::QuietDelete(f1, allowed_filedrv));
  EXPECT_NE(0, VSIStatL(f1, &sStat));

  // Delete(poDS, papszFileList) overload on existing and nonexistent files:
  const char* const nonexist_files[] = {"/vsimem/no_such_unlink.dat", nullptr};
  EXPECT_EQ(CE_Failure, drv.Delete(nullptr, nonexist_files));

  // Custom pfnCopyFiles, pfnRename, pfnDelete, pfnDeleteDataSource.
  drv.pfnCopyFiles = [](const char*, const char*) -> CPLErr { return CE_None; };
  drv.pfnRename = [](const char*, const char*) -> CPLErr { return CE_None; };
  drv.pfnDelete = [](const char*) -> CPLErr { return CE_None; };
  EXPECT_EQ(CE_None, drv.CopyFiles("a", "b"));
  EXPECT_EQ(CE_None, drv.Rename("a", "b"));
  EXPECT_EQ(CE_None, drv.Delete("a"));

  drv.pfnDelete = nullptr;
  drv.pfnDeleteDataSource = [](GDALDriver*, const char*) -> CPLErr {
    return CE_None;
  };
  EXPECT_EQ(CE_None, drv.Delete("a"));

  // CanVectorTranslateFrom & VectorTranslateFrom.
  DummyDataset src(2, 2, 1);
  char** reasons = nullptr;
  EXPECT_FALSE(drv.CanVectorTranslateFrom("dst", &src, nullptr, &reasons));
  CSLDestroy(reasons);
  EXPECT_EQ(nullptr,
            drv.VectorTranslateFrom("dst", &src, nullptr, nullptr, nullptr));

  drv.pfnCanVectorTranslateFrom = [](const char*, GDALDataset*, CSLConstList,
                                     char***) -> bool { return true; };
  drv.pfnVectorTranslateFrom = [](const char*, GDALDataset*, CSLConstList,
                                  GDALProgressFunc, void*) -> GDALDataset* {
    return new DummyDataset(1, 1, 1);
  };
  EXPECT_TRUE(drv.CanVectorTranslateFrom("dst", &src, nullptr, nullptr));
  std::unique_ptr<GDALDataset> vt_ds(
      drv.VectorTranslateFrom("dst", &src, nullptr, nullptr, nullptr));
  EXPECT_NE(nullptr, vt_ds);

  GetGDALDriverManager()->DeregisterDriver(&drv);
  CPLPopErrorHandler();
}

TEST(GdalDriverTest, IdentifyDriverAndAlgorithmsAndOutputDrivers) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Custom driver with pfnIdentifyEx and DeclareAlgorithm.
  GDALDriver custom_drv;
  custom_drv.SetDescription("CUST_ALG_DRV");
  custom_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  custom_drv.SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/cust.html");
  custom_drv.pfnIdentifyEx = [](GDALDriver*, GDALOpenInfo* poOpenInfo) -> int {
    return EQUAL(poOpenInfo->pszFilename, "CUST_MAGIC:") ? 1 : 0;
  };
  custom_drv.pfnInstantiateAlgorithm =
      [](const std::vector<std::string>&) -> GDALAlgorithm* {
    return new TestAlgo();
  };

  GetGDALDriverManager()->RegisterDriver(&custom_drv);

  EXPECT_EQ(GDALDriver::ToHandle(&custom_drv),
            GDALIdentifyDriver("CUST_MAGIC:", nullptr));
  const char* const allowed[] = {"CUST_ALG_DRV", nullptr};
  EXPECT_EQ(
      GDALDriver::ToHandle(&custom_drv),
      GDALIdentifyDriverEx("CUST_MAGIC:", GDAL_OF_RASTER, allowed, nullptr));

  custom_drv.DeclareAlgorithm({"test-op"});
  auto& reg = GDALGlobalAlgorithmRegistry::GetSingleton();
  auto drv_alg = reg.Instantiate({"driver", "cust_alg_drv", "test-op"});
  EXPECT_NE(nullptr, drv_alg);

  // Output drivers for dataset names.
  // Also test third-pass slow identify (driver with pfnIdentify == nullptr and
  // pfnOpen != nullptr):
  GDALDriver slow_drv;
  slow_drv.SetDescription("SLOW_OPEN_DRV");
  slow_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  slow_drv.pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
    if (EQUAL(poOpenInfo->pszFilename, "SLOW_MAGIC:")) {
      return new DummyDataset(1, 1, 1);
    }
    return nullptr;
  };
  GetGDALDriverManager()->RegisterDriver(&slow_drv);
  const char* const slow_allowed[] = {"SLOW_OPEN_DRV", nullptr};
  EXPECT_EQ(GDALDriver::ToHandle(&slow_drv),
            GDALIdentifyDriverEx("SLOW_MAGIC:", GDAL_OF_RASTER, slow_allowed,
                                 nullptr));
  GetGDALDriverManager()->DeregisterDriver(&slow_drv);

  // Known connection prefix warning when driver is not enabled:
  char** oci_drvs = GDALGetOutputDriversForDatasetName(
      "OCI:user/pass@db", GDAL_OF_VECTOR, true, true);
  CSLDestroy(oci_drvs);

  // GDB single-match special case:
  char** gdb_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/out.gdb", GDAL_OF_VECTOR, true, true);
  CSLDestroy(gdb_drvs);

  // Multiple vector drivers matching same extension/prefix warning:
  GDALDriver dup1, dup2;
  dup1.SetDescription("DUP_VEC1");
  dup1.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  dup1.SetMetadataItem(GDAL_DCAP_CREATE, "YES");
  dup1.SetMetadataItem(GDAL_DMD_EXTENSIONS, "dupext");
  dup1.SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, "DUPPRE:");
  dup2.SetDescription("DUP_VEC2");
  dup2.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  dup2.SetMetadataItem(GDAL_DCAP_CREATE, "YES");
  dup2.SetMetadataItem(GDAL_DMD_EXTENSIONS, "dupext");
  dup2.SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, "DUPPRE:");
  GetGDALDriverManager()->RegisterDriver(&dup1);
  GetGDALDriverManager()->RegisterDriver(&dup2);
  char** dup_ext_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/out.dupext", GDAL_OF_VECTOR, true, true);
  CSLDestroy(dup_ext_drvs);
  char** dup_pre_drvs = GDALGetOutputDriversForDatasetName(
      "DUPPRE:foo", GDAL_OF_VECTOR, true, true);
  CSLDestroy(dup_pre_drvs);
  GetGDALDriverManager()->DeregisterDriver(&dup2);
  GetGDALDriverManager()->DeregisterDriver(&dup1);
  char** tif_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/out.tif", GDAL_OF_RASTER, /*bSingleMatch=*/true,
      /*bEmitWarning=*/true);
  EXPECT_NE(nullptr, tif_drvs);
  CSLDestroy(tif_drvs);

  char** noext_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/out_no_ext", GDAL_OF_RASTER, /*bSingleMatch=*/true,
      /*bEmitWarning=*/true);
  EXPECT_NE(nullptr, noext_drvs);
  CSLDestroy(noext_drvs);

  char** shp_zip_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/out.shp.zip", GDAL_OF_VECTOR, false, false);
  CSLDestroy(shp_zip_drvs);

  char** gpkg_zip_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/out.gpkg.zip", GDAL_OF_VECTOR, false, false);
  CSLDestroy(gpkg_zip_drvs);

  EXPECT_EQ(nullptr, GDALGetOutputDriversForDatasetName(
                         "/vsimem/out.gdalg.json", GDAL_OF_RASTER, true, true));

  // Missing plugin driver message via a dummy driver with
  // MISSING_PLUGIN_FILENAME.
  GDALDriver missing_drv;
  missing_drv.SetDescription("MISSING_FOO");
  missing_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  missing_drv.SetMetadataItem(GDAL_DCAP_CREATE, "YES");
  missing_drv.SetMetadataItem(GDAL_DMD_EXTENSIONS, "missingfoo");
  missing_drv.SetMetadataItem("MISSING_PLUGIN_FILENAME", "gdal_MISSING.so");
  missing_drv.SetMetadataItem(GDAL_DMD_PLUGIN_INSTALLATION_MESSAGE,
                              "Install foo-pkg.");
  GetGDALDriverManager()->RegisterDriver(&missing_drv);

  char** missing_drvs = GDALGetOutputDriversForDatasetName(
      "/vsimem/test.missingfoo", GDAL_OF_RASTER, true, true);
  CSLDestroy(missing_drvs);

  CPLSetConfigOption("GDAL_DRIVER_PATH", "/vsimem/nonexistent_plugin_dir");
  missing_drvs = GDALGetOutputDriversForDatasetName("/vsimem/test.missingfoo",
                                                    GDAL_OF_RASTER, true, true);
  CSLDestroy(missing_drvs);
  CPLSetConfigOption("GDAL_DRIVER_PATH", nullptr);

  // GDALDeleteDataset and GDALRenameDataset with hDriver == nullptr on
  // unidentifiable file:
  EXPECT_EQ(GDALDeleteDataset(nullptr, "/vsimem/no_such_identifiable_file.xyz"),
            CE_Failure);
  EXPECT_EQ(GDALRenameDataset(nullptr, "/vsimem/new.xyz",
                              "/vsimem/no_such_identifiable_file.xyz"),
            CE_Failure);
  DummyDataset del_ds(1, 1, 1);
  del_ds.SetDescription("/vsimem/del_ds_cb.bin");
  custom_drv.pfnDelete = [](const char*) -> CPLErr { return CE_None; };
  EXPECT_EQ(custom_drv.Delete(&del_ds, nullptr), CE_None);
  custom_drv.pfnDelete = nullptr;

  GetGDALDriverManager()->DeregisterDriver(&missing_drv);
  GetGDALDriverManager()->DeregisterDriver(&custom_drv);
  CPLPopErrorHandler();
}

}  // namespace
}  // namespace gdal
