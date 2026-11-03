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
// Tests GDALDriver and related driver functions (gcore/gdaldriver.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with PAM_ENABLED
// undefined (GDAL_PAM_ENABLED defaults to "NO") and -DGDAL_NO_AUTOLOAD.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaldriver.cpp

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr char kVsiDir[] = "/vsimem/gdaldriver_test";

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
    data_.resize(static_cast<std::size_t>(xsize) * ysize, 7);
  }

  CPLErr IReadBlock(int, int nBlockYOff, void* pImage) override {
    std::memcpy(pImage,
                data_.data() + static_cast<std::size_t>(nBlockYOff) *
                                   static_cast<std::size_t>(nRasterXSize),
                static_cast<std::size_t>(nRasterXSize));
    return CE_None;
  }

  CPLErr IWriteBlock(int, int nBlockYOff, void* pImage) override {
    std::memcpy(data_.data() + static_cast<std::size_t>(nBlockYOff) *
                                   static_cast<std::size_t>(nRasterXSize),
                pImage, static_cast<std::size_t>(nRasterXSize));
    return CE_None;
  }

  CPLErr CreateMaskBand(int flags) override {
    mask_band_ =
        std::make_unique<DummyBand>(poDS, 1, nRasterXSize, nRasterYSize);
    mask_flags_ = flags;
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

  CPLErr SetColorTable(GDALColorTable* ct) override {
    if (ct != nullptr) {
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
    if (pbSuccess != nullptr) *pbSuccess = has_ndv_ ? 1 : 0;
    return ndv_;
  }

  CPLErr SetOffset(double val) override {
    has_offset_ = true;
    offset_ = val;
    return CE_None;
  }
  double GetOffset(int* pbSuccess) override {
    if (pbSuccess != nullptr) *pbSuccess = has_offset_ ? 1 : 0;
    return offset_;
  }

  CPLErr SetScale(double val) override {
    has_scale_ = true;
    scale_ = val;
    return CE_None;
  }
  double GetScale(int* pbSuccess) override {
    if (pbSuccess != nullptr) *pbSuccess = has_scale_ ? 1 : 0;
    return scale_;
  }

  CPLErr SetUnitType(const char* unit) override {
    unit_ = unit != nullptr ? unit : "";
    return CE_None;
  }
  const char* GetUnitType() override { return unit_.c_str(); }

  CPLErr SetCategoryNames(char** names) override {
    cats_.Assign(CSLDuplicate(names), /*bTakeOwnership=*/TRUE);
    return CE_None;
  }
  char** GetCategoryNames() override { return cats_.List(); }

  CPLErr SetDefaultRAT(const GDALRasterAttributeTable* rat) override {
    if (rat != nullptr) {
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
      SetBand(i, std::make_unique<DummyBand>(this, i, xsize, ysize));
    }
  }

  ~DummyDataset() override {
    if (gcp_count_ > 0) {
      GDALDeinitGCPs(gcp_count_, gcps_);
      CPLFree(gcps_);
    }
  }

  void SetDriver(GDALDriver* drv) { poDriver = drv; }

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
    if (srs != nullptr) {
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
    if (poGCP_SRS != nullptr) {
      gcp_srs_ = *poGCP_SRS;
    } else {
      gcp_srs_.Clear();
    }
    return CE_None;
  }
  int GetGCPCount() override { return gcp_count_; }
  const GDAL_GCP* GetGCPs() override { return gcps_; }
  const OGRSpatialReference* GetGCPSpatialRef() const override {
    return gcp_srs_.IsEmpty() ? nullptr : &gcp_srs_;
  }

  CPLErr CreateMaskBand(int flags) override {
    for (int i = 1; i <= nBands; ++i) {
      GetRasterBand(i)->CreateMaskBand(flags);
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
    if (i < 0 || static_cast<std::size_t>(i) >= layers_.size()) return nullptr;
    return layers_[i];
  }
  int TestCapability(const char* cap) const override {
    if (EQUAL(cap, ODsCCreateLayer)) return can_create_layer_ ? 1 : 0;
    return 0;
  }
  OGRLayer* CopyLayer(OGRLayer* src_layer, const char*, char**) override {
    ++copied_layers_;
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
                const std::shared_ptr<GDALGroup>&, bool, GUInt64&, GUInt64,
                GDALProgressFunc, void*, CSLConstList) override {
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
  GDALRegister_GTiff();

  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALGetDriverShortName(nullptr));
    EXPECT_EQ(nullptr, GDALGetDriverLongName(nullptr));
    EXPECT_EQ(nullptr, GDALGetDriverHelpTopic(nullptr));
    EXPECT_EQ(nullptr, GDALGetDriverCreationOptionList(nullptr));
    CPLErrorReset();
  }

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
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("GTiff"));
  driver2->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  const char* ovr_xml =
      driver2->GetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST);
  EXPECT_NE(nullptr, ovr_xml);
  driver2->SetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST, "<Custom/>");
  EXPECT_STREQ("<Custom/>",
               driver2->GetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST));

  GDALDestroyDriver(GDALDriver::ToHandle(driver.release()));
}

TEST(GdalDriverTest, ValidateOptionsAllTypesAndBranches) {
  GDALDriver drv;
  drv.SetDescription("OPTDRV");

  constexpr char kOptionXML[] =
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
  constexpr char kOpenOptionXML[] =
      "<OpenOptionList>"
      "  <Option name='OPEN_A' type='STRING' alias='OPEN_ALIAS'/>"
      "</OpenOptionList>";

  drv.SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, kOptionXML);
  drv.SetMetadataItem(GDAL_DMD_OPENOPTIONLIST, kOpenOptionXML);

  EXPECT_TRUE(drv.HasOpenOption("OPEN_A"));
  EXPECT_TRUE(GDALDriverHasOpenOption(GDALDriver::ToHandle(&drv), "OPEN_A"));
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(GDALDriverHasOpenOption(nullptr, "OPEN_A"));
    CPLErrorReset();
  }
  EXPECT_FALSE(drv.HasOpenOption(nullptr));
  EXPECT_FALSE(drv.HasOpenOption("OPEN_ALIAS"));
  EXPECT_FALSE(drv.HasOpenOption("NO_SUCH_OPT"));

  GDALDriverH hDrv = GDALDriver::ToHandle(&drv);
  EXPECT_TRUE(GDALValidateCreationOptions(hDrv, nullptr));
  constexpr const char* kOpenOptsVal[] = {"OPEN_A=ok", "UNKNOWN_OPEN=1",
                                          "VALIDATE_OPEN_OPTIONS=NO", nullptr};
  EXPECT_TRUE(GDALValidateOpenOptions(hDrv, kOpenOptsVal));

  // Valid options (including wildcards PREFIX_*, *_SUFFIX, MID_*_END).
  constexpr const char* kValidOpts[] = {
      "INT_OPT=+42",    "INT_ALIAS=-5",   "INT_OLD=10",
      "UINT_OPT=25",    "FLOAT_OPT=3.25", "BOOL_OPT=YES",
      "BOOL_OPT=OFF",   "SEL_OPT=A",      "SEL_OPT=VAL_A",
      "SEL_OPT=OLD_A",  "STR_OPT=abcd",   "PREFIX_FOO=bar",
      "FOO_SUFFIX=bar", "MID_X_END=bar",  "APPEND_SUBDATASET=YES",
      "@silent_opt=1",  nullptr};
  EXPECT_TRUE(GDALValidateCreationOptions(hDrv, kValidOpts));

  // Invalid options for each type and range check.
  constexpr const char* kInvalidOpts[] = {"NO_EQUAL_SIGN",
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
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(GDALValidateCreationOptions(hDrv, kInvalidOpts));
    CPLErrorReset();
  }

  // Invalid XML in CreationOptionList.
  drv.SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, "<BrokenXML");
  constexpr const char* kOneOpt[] = {"INT_OPT=1", nullptr};
  {
    WithQuietHandler quiet;
    EXPECT_TRUE(GDALValidateCreationOptions(hDrv, kOneOpt));
    CPLErrorReset();
  }
}

TEST(GdalDriverTest, CreateAndCreateMultiDimensionalAndOpen) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  constexpr char kTestFile1[] = "/vsimem/gdaldriver_test/drv_test1.bin";
  constexpr char kTestFile2[] = "/vsimem/gdaldriver_test/drv_test2.bin";
  constexpr char kVecFile[] = "/vsimem/gdaldriver_test/drv_vec.bin";
  constexpr char kMdimFile[] = "/vsimem/gdaldriver_test/mdim.bin";

  GDALDriver drv;
  drv.SetDescription("TESTDRV");

  // 1. Unsupported Create when no callbacks are set.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, drv.Create(kTestFile1, 10, 10, 1, GDT_Byte, nullptr));
    CPLErrorReset();
  }

  // 2. Invalid dimensions/types (when GDAL_DCAP_RASTER is set and not VECTOR).
  drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  drv.pfnCreate = [](const char*, int x, int y, int b, GDALDataType,
                     char**) -> GDALDataset* {
    return new DummyDataset(x, y, b);
  };
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, drv.Create(kTestFile1, 0, 10, 1, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv.Create(kTestFile1, 10, -1, 1, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv.Create(kTestFile1, 10, 10, -1, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv.Create(kTestFile1, 10, 10, 1, GDT_Unknown, nullptr));
    CPLErrorReset();
  }

  // 3. Valid pfnCreate.
  GDALDatasetUniquePtr ds1(drv.Create(kTestFile1, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds1);
  EXPECT_EQ(&drv, ds1->GetDriver());
  EXPECT_EQ(GA_Update, ds1->GetAccess());

  // 4. pfnCreateEx path.
  drv.pfnCreate = nullptr;
  drv.pfnCreateEx = [](GDALDriver*, const char*, int x, int y, int b,
                       GDALDataType, char**) -> GDALDataset* {
    return new DummyDataset(x, y, b);
  };
  GDALDatasetUniquePtr ds2(drv.Create(kTestFile2, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds2);

  // 5. pfnCreateVectorOnly path.
  drv.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  drv.pfnCreateEx = nullptr;
  drv.pfnCreateVectorOnly = [](GDALDriver*, const char*,
                               char**) -> GDALDataset* {
    return new DummyDataset(0, 0, 0);
  };
  GDALDatasetUniquePtr ds3(GDALDataset::FromHandle(GDALCreate(
      GDALDriver::ToHandle(&drv), kVecFile, 0, 0, 0, GDT_Unknown, nullptr)));
  ASSERT_NE(nullptr, ds3);

  // 6. CreateMultiDimensional.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, drv.CreateMultiDimensional(kMdimFile, nullptr, nullptr));
    CPLErrorReset();
  }
  drv.pfnCreateMultiDimensional = [](const char*, CSLConstList,
                                     CSLConstList) -> GDALDataset* {
    auto ds = std::make_unique<DummyDataset>(0, 0, 0);
    ds->root_group_ = std::make_shared<DummyGroup>();
    return ds.release();
  };
  constexpr char kMultiDimGroupOptionXML[] =
      "<MultiDimGroupCreationOptionList>"
      "  <Option name='GRP_OPT' type='STRING'/>"
      "</MultiDimGroupCreationOptionList>";
  drv.SetMetadataItem(GDAL_DMD_MULTIDIM_GROUP_CREATIONOPTIONLIST,
                      kMultiDimGroupOptionXML);
  constexpr const char* kRootGroupOpts[] = {"GRP_OPT=val", nullptr};
  GDALDatasetUniquePtr mdim_ds(
      GDALDataset::FromHandle(GDALCreateMultiDimensional(
          GDALDriver::ToHandle(&drv), kMdimFile, kRootGroupOpts, nullptr)));
  ASSERT_NE(nullptr, mdim_ds);

  // 7. Open via pfnOpen and pfnOpenWithDriverArg.
  GDALOpenInfo open_info(kTestFile1, GA_ReadOnly);
  CPLStringList open_opts({"OPEN_OPT=1"});
  open_info.papszOpenOptions = open_opts.List();
  EXPECT_EQ(nullptr, drv.Open(&open_info, true));

  drv.pfnOpen = [](GDALOpenInfo*) -> GDALDataset* {
    return new DummyDataset(2, 2, 1);
  };
  GDALDatasetUniquePtr opened1(drv.Open(&open_info, true));
  ASSERT_NE(nullptr, opened1);

  drv.pfnOpen = nullptr;
  drv.pfnOpenWithDriverArg = [](GDALDriver*, GDALOpenInfo*) -> GDALDataset* {
    return new DummyDataset(2, 2, 1);
  };
  GDALDatasetUniquePtr opened2(drv.Open(&open_info, false));
  ASSERT_NE(nullptr, opened2);
}

TEST(GdalDriverTest, DefaultCreateCopyAndMetadataAndMasks) {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

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
  absl::Cleanup deinit_gcp = [&] { GDALDeinitGCPs(1, &gcp); };
  gcp.dfGCPPixel = 1.0;
  gcp.dfGCPLine = 2.0;
  gcp.dfGCPX = 11.0;
  gcp.dfGCPY = 18.0;
  src.SetGCPs(1, &gcp, &srs);

  auto* b1 = cpl::down_cast<DummyBand*>(src.GetRasterBand(1));
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
  CPLStringList cats({"CatA", "CatB"});
  b1->SetCategoryNames(cats.List());
  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Value", GFT_Integer, GFU_MinMax);
  rat.SetValue(0, 0, 1);
  b1->SetDefaultRAT(&rat);
  b1->SetMetadataItem("BKEY", "BVAL");
  b1->SetMetadataItem("BCUSTOM", "BCVAL", "BAND_DOM");
  b1->CreateMaskBand(GMF_PER_DATASET);

  // CreateCopy using DefaultCreateCopy (since pfnCreateCopy is nullptr).
  constexpr const char* kCopyMddOpts[] = {"COPY_SRC_MDD=YES", nullptr};
  GDALDatasetUniquePtr dst(GDALDataset::FromHandle(GDALCreateCopy(
      GDALDriver::ToHandle(&drv), "/vsimem/gdaldriver_test/copy_dst.bin",
      GDALDataset::ToHandle(&src), FALSE, kCopyMddOpts, nullptr, nullptr)));
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
  EXPECT_THAT(CslToVector(dst->GetRasterBand(1)->GetCategoryNames()),
              ElementsAre("CatA", "CatB"));
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
    auto ds = std::make_unique<DummyDataset>(0, 0, 0);
    ds->root_group_ = std::make_shared<DummyGroup>();
    return ds.release();
  };
  GDALDatasetUniquePtr mdim_dst(
      drv.CreateCopy("/vsimem/gdaldriver_test/mdim_copy.bin", &mdim_src, FALSE,
                     nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, mdim_dst);

  // Test pfnCreateCopy custom callback and APPEND_SUBDATASET=YES.
  drv.pfnCreateCopy = [](const char*, GDALDataset*, int, char**,
                         GDALProgressFunc, void*) -> GDALDataset* {
    return new DummyDataset(2, 2, 1);
  };
  constexpr const char* kAppendOpts[] = {"APPEND_SUBDATASET=YES",
                                         "_INTERNAL_DATASET=YES", nullptr};
  GDALDatasetUniquePtr copy2(drv.CreateCopy("/vsimem/gdaldriver_test/copy2.bin",
                                            &src, FALSE, kAppendOpts, nullptr,
                                            nullptr));
  ASSERT_NE(nullptr, copy2);

  // Additional DefaultCreateCopy branches:
  // 1. Automatic INTERLEAVE propagation + NBITS/PIXELTYPE propagation +
  //    @QUIET_DELETE_ON_CREATE_COPY=NO:
  drv.pfnCreateCopy = nullptr;
  drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  constexpr char kCopyOptionXML[] =
      "<CreationOptionList>"
      "  <Option name='INTERLEAVE' type='STRING-SELECT'>"
      "    <Value>BAND</Value><Value>LINE</Value><Value>PIXEL</Value>"
      "  </Option>"
      "  <Option name='NBITS' type='INT'/>"
      "  <Option name='PIXELTYPE' type='STRING'/>"
      "</CreationOptionList>";
  drv.SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, kCopyOptionXML);
  src.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  b1->SetMetadataItem("NBITS", "4", "IMAGE_STRUCTURE");
  b1->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  constexpr const char* kQuietDeleteOpts[] = {"@QUIET_DELETE_ON_CREATE_COPY=NO",
                                              "SKIP_HOLES=YES", nullptr};
  GDALDatasetUniquePtr copy3(drv.CreateCopy("/vsimem/gdaldriver_test/copy3.bin",
                                            &src, FALSE, kQuietDeleteOpts,
                                            nullptr, nullptr));
  ASSERT_NE(nullptr, copy3);

  // 2. Progress callback returning 0 at start of DefaultCreateCopy:
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        nullptr,
        drv.CreateCopy(
            "/vsimem/gdaldriver_test/copy_cancel.bin", &src, FALSE, nullptr,
            [](double, const char*, void*) -> int { return 0; }, nullptr));
    CPLErrorReset();
  }

  // 3. 0-band non-vector dataset failure in DefaultCreateCopy:
  DummyDataset zero_ds(4, 4, 0);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              drv.CreateCopy("/vsimem/gdaldriver_test/copy_zero.bin", &zero_ds,
                             FALSE, nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  // 4. Raster-only vs Vector-only driver mismatch in DefaultCreateCopy:
  GDALDriver ras_only_drv;
  ras_only_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  GDALDriver vec_only_drv;
  vec_only_drv.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  {
    WithQuietHandler quiet;
    src.SetDriver(&ras_only_drv);
    EXPECT_EQ(nullptr, vec_only_drv.DefaultCreateCopy(
                           "/vsimem/gdaldriver_test/mismatch1", &src, FALSE,
                           nullptr, nullptr, nullptr));
    src.SetDriver(&vec_only_drv);
    EXPECT_EQ(nullptr, ras_only_drv.DefaultCreateCopy(
                           "/vsimem/gdaldriver_test/mismatch2", &src, FALSE,
                           nullptr, nullptr, nullptr));
    CPLErrorReset();
  }
  src.SetDriver(nullptr);

  // 5. Output driver creating wrong number of bands (nDstBands != nBands):
  GDALDriver bad_bands_drv;
  bad_bands_drv.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  bad_bands_drv.pfnCreate = [](const char*, int x, int y, int, GDALDataType,
                               char**) -> GDALDataset* {
    return new DummyDataset(x, y, 1);  // Returns 1 band when 2 requested.
  };
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, bad_bands_drv.DefaultCreateCopy(
                           "/vsimem/gdaldriver_test/bad_bands.bin", &src, FALSE,
                           nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  // 6. Vector layers copy in DefaultCreateCopy when nDstBands == 0:
  GDALDriver vec_copy_drv;
  vec_copy_drv.SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  vec_copy_drv.pfnCreate = [](const char*, int, int, int, GDALDataType,
                              char**) -> GDALDataset* {
    auto d = std::make_unique<DummyDataset>(0, 0, 0);
    d->can_create_layer_ = true;
    return d.release();
  };
  GDALDriver* mem_vec_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_vec_drv);
  GDALDatasetUniquePtr mem_vds(
      mem_vec_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, mem_vds);
  OGRLayer* lyr = mem_vds->CreateLayer("lyr1", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);
  DummyDataset vsrc(0, 0, 0);
  vsrc.SetGeoTransform(gt);
  vsrc.SetSpatialRef(&srs);
  vsrc.layers_.push_back(lyr);
  vsrc.layers_.push_back(nullptr);
  GDALDatasetUniquePtr vdst(
      vec_copy_drv.DefaultCreateCopy("/vsimem/gdaldriver_test/vec_copy.bin",
                                     &vsrc, FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, vdst);
  EXPECT_EQ(1, cpl::down_cast<DummyDataset*>(vdst.get())->copied_layers_);

  // 7. QuietDeleteForCreateCopy when source and dest share a file:
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  constexpr char kShareTifPath[] = "/vsimem/gdaldriver_test/qd_share.tif";
  constexpr char kShareAuxPath[] =
      "/vsimem/gdaldriver_test/qd_share.tif.aux.xml";
  GDALDatasetUniquePtr t1(
      gtiff_drv->Create(kShareTifPath, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, t1);
  t1->SetMetadataItem("K", "V");
  t1.reset();
  VSILFILE* aux_fp = VSIFOpenL(kShareAuxPath, "wb");
  ASSERT_NE(nullptr, aux_fp);
  EXPECT_EQ(0, VSIFCloseL(aux_fp));
  GDALDatasetUniquePtr t1_open(
      GDALDataset::Open(kShareTifPath, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, t1_open);
  GDALDatasetUniquePtr t2(gtiff_drv->CreateCopy(
      kShareTifPath, t1_open.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, t2);
}

TEST(GdalDriverTest, DeleteRenameCopyFilesAndVectorTranslate) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  constexpr char kF1[] = "/vsimem/gdaldriver_test/drv_op_1.dat";
  constexpr char kF1Aux[] = "/vsimem/gdaldriver_test/drv_op_1.dat.aux.xml";
  constexpr char kF2[] = "/vsimem/gdaldriver_test/drv_op_2.dat";
  constexpr char kF2Aux[] = "/vsimem/gdaldriver_test/drv_op_2.dat.aux.xml";
  constexpr char kF3[] = "/vsimem/gdaldriver_test/drv_op_3.dat";

  VSILFILE* fp1 = VSIFOpenL(kF1, "wb");
  ASSERT_NE(nullptr, fp1);
  EXPECT_EQ(0, VSIFCloseL(fp1));
  VSILFILE* fp1_aux = VSIFOpenL(kF1Aux, "wb");
  ASSERT_NE(nullptr, fp1_aux);
  EXPECT_EQ(0, VSIFCloseL(fp1_aux));

  auto drv = std::make_unique<GDALDriver>();
  drv->SetDescription("FILEDRV");
  drv->pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
    VSIStatBufL sStat;
    if (VSIStatL(poOpenInfo->pszFilename, &sStat) != 0) return nullptr;
    auto ds = std::make_unique<DummyDataset>(2, 2, 1);
    ds->SetDescription(poOpenInfo->pszFilename);
    ds->file_list_.AddString(poOpenInfo->pszFilename);
    std::string aux = std::string(poOpenInfo->pszFilename) + ".aux.xml";
    if (VSIStatL(aux.c_str(), &sStat) == 0) {
      ds->file_list_.AddString(aux.c_str());
    }
    return ds.release();
  };
  drv->pfnIdentify = [](GDALOpenInfo* poOpenInfo) -> int {
    return std::string_view(poOpenInfo->pszFilename).find("drv_op_") !=
                   std::string_view::npos
               ? 1
               : 0;
  };

  GetGDALDriverManager()->RegisterDriver(drv.get());
  absl::Cleanup deregister_drv = [&] {
    GetGDALDriverManager()->DeregisterDriver(drv.get());
  };
  GDALDriverH hDrv = GDALDriver::ToHandle(drv.get());

  // CopyDatasetFiles (using DefaultCopyFiles).
  EXPECT_EQ(CE_None, GDALCopyDatasetFiles(hDrv, kF2, kF1));
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(kF2, &sStat));
  EXPECT_EQ(0, VSIStatL(kF2Aux, &sStat));

  // RenameDataset (using DefaultRename).
  EXPECT_EQ(CE_None, GDALRenameDataset(hDrv, kF3, kF2));
  EXPECT_EQ(0, VSIStatL(kF3, &sStat));
  EXPECT_NE(0, VSIStatL(kF2, &sStat));

  // DeleteDataset via GetFileList() when pfnDelete is nullptr.
  EXPECT_EQ(CE_None, GDALDeleteDataset(hDrv, kF3));
  EXPECT_NE(0, VSIStatL(kF3, &sStat));

  // QuietDelete with allowed drivers on nonexistent kF2 (returns CE_Failure
  // since pfnIdentify matches "drv_op_" and Delete fails to open kF2) and on
  // existing kF1 (returns CE_None and unlinks kF1).
  constexpr const char* kAllowedFileDrv[] = {"FILEDRV", nullptr};
  EXPECT_EQ(CE_Failure, GDALDriver::QuietDelete(kF2, kAllowedFileDrv));
  EXPECT_EQ(CE_None, GDALDriver::QuietDelete(kF1, kAllowedFileDrv));
  EXPECT_NE(0, VSIStatL(kF1, &sStat));

  // Delete(poDS, papszFileList) overload on nonexistent files.
  constexpr const char* kNonexistFiles[] = {
      "/vsimem/gdaldriver_test/no_such_unlink.dat", nullptr};
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, drv->Delete(nullptr, kNonexistFiles));
    CPLErrorReset();
  }

  // Custom pfnCopyFiles, pfnRename, pfnDelete, pfnDeleteDataSource.
  drv->pfnCopyFiles = [](const char*, const char*) -> CPLErr {
    return CE_None;
  };
  drv->pfnRename = [](const char*, const char*) -> CPLErr { return CE_None; };
  drv->pfnDelete = [](const char*) -> CPLErr { return CE_None; };
  EXPECT_EQ(CE_None, drv->CopyFiles("a", "b"));
  EXPECT_EQ(CE_None, drv->Rename("a", "b"));
  EXPECT_EQ(CE_None, drv->Delete("a"));

  drv->pfnDelete = nullptr;
  drv->pfnDeleteDataSource = [](GDALDriver*, const char*) -> CPLErr {
    return CE_None;
  };
  EXPECT_EQ(CE_None, drv->Delete("a"));

  // CanVectorTranslateFrom & VectorTranslateFrom.
  DummyDataset src(2, 2, 1);
  char** reasons_raw = nullptr;
  EXPECT_FALSE(drv->CanVectorTranslateFrom("dst", &src, nullptr, &reasons_raw));
  StringListPtr reasons(reasons_raw, CSLDestroy);
  EXPECT_FALSE(CslToVector(reasons.get()).empty());
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              drv->VectorTranslateFrom("dst", &src, nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  drv->pfnCanVectorTranslateFrom = [](const char*, GDALDataset*, CSLConstList,
                                      char***) -> bool { return true; };
  drv->pfnVectorTranslateFrom = [](const char*, GDALDataset*, CSLConstList,
                                   GDALProgressFunc, void*) -> GDALDataset* {
    return new DummyDataset(1, 1, 1);
  };
  EXPECT_TRUE(drv->CanVectorTranslateFrom("dst", &src, nullptr, nullptr));
  GDALDatasetUniquePtr vt_ds(
      drv->VectorTranslateFrom("dst", &src, nullptr, nullptr, nullptr));
  EXPECT_NE(nullptr, vt_ds);
}

TEST(GdalDriverTest, IdentifyDriverAndAlgorithmsAndOutputDrivers) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  // Custom driver with pfnIdentifyEx and DeclareAlgorithm.
  auto custom_drv = std::make_unique<GDALDriver>();
  custom_drv->SetDescription("CUST_ALG_DRV");
  custom_drv->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  custom_drv->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/cust.html");
  custom_drv->pfnIdentifyEx = [](GDALDriver*, GDALOpenInfo* poOpenInfo) -> int {
    return EQUAL(poOpenInfo->pszFilename, "CUST_MAGIC:") ? 1 : 0;
  };
  custom_drv->pfnInstantiateAlgorithm =
      [](const std::vector<std::string>&) -> GDALAlgorithm* {
    return new TestAlgo();
  };

  GetGDALDriverManager()->RegisterDriver(custom_drv.get());
  absl::Cleanup deregister_custom_drv = [&] {
    GetGDALDriverManager()->DeregisterDriver(custom_drv.get());
  };

  EXPECT_EQ(GDALDriver::ToHandle(custom_drv.get()),
            GDALIdentifyDriver("CUST_MAGIC:", nullptr));
  constexpr const char* kAllowed[] = {"CUST_ALG_DRV", nullptr};
  EXPECT_EQ(
      GDALDriver::ToHandle(custom_drv.get()),
      GDALIdentifyDriverEx("CUST_MAGIC:", GDAL_OF_RASTER, kAllowed, nullptr));

  custom_drv->DeclareAlgorithm({"test-op"});
  auto& reg = GDALGlobalAlgorithmRegistry::GetSingleton();
  auto drv_alg = reg.Instantiate({"driver", "cust_alg_drv", "test-op"});
  EXPECT_NE(nullptr, drv_alg);

  // Third-pass slow identify (driver with pfnIdentify == nullptr and
  // pfnOpen != nullptr).
  {
    auto slow_drv = std::make_unique<GDALDriver>();
    slow_drv->SetDescription("SLOW_OPEN_DRV");
    slow_drv->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    slow_drv->pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
      if (EQUAL(poOpenInfo->pszFilename, "SLOW_MAGIC:")) {
        return new DummyDataset(1, 1, 1);
      }
      return nullptr;
    };
    GetGDALDriverManager()->RegisterDriver(slow_drv.get());
    absl::Cleanup deregister_slow_drv = [&] {
      GetGDALDriverManager()->DeregisterDriver(slow_drv.get());
    };
    constexpr const char* kSlowAllowed[] = {"SLOW_OPEN_DRV", nullptr};
    EXPECT_EQ(GDALDriver::ToHandle(slow_drv.get()),
              GDALIdentifyDriverEx("SLOW_MAGIC:", GDAL_OF_RASTER, kSlowAllowed,
                                   nullptr));
  }

  // Known connection prefix warning/error when driver is not enabled.
  {
    WithQuietHandler quiet;
    StringListPtr oci_drvs(
        GDALGetOutputDriversForDatasetName("OCI:user/pass@db", GDAL_OF_VECTOR,
                                           /*bSingleMatch=*/true,
                                           /*bEmitWarning=*/true),
        CSLDestroy);
    EXPECT_THAT(CslToVector(oci_drvs.get()), IsEmpty());
    CPLErrorReset();
  }

  // GDB single-match special case.
  StringListPtr gdb_drvs(GDALGetOutputDriversForDatasetName(
                             "/vsimem/gdaldriver_test/out.gdb", GDAL_OF_VECTOR,
                             /*bSingleMatch=*/true,
                             /*bEmitWarning=*/true),
                         CSLDestroy);
  EXPECT_THAT(CslToVector(gdb_drvs.get()), ElementsAre("OpenFileGDB"));

  // Multiple vector drivers matching same extension/prefix warning.
  {
    auto dup1 = std::make_unique<GDALDriver>();
    dup1->SetDescription("DUP_VEC1");
    dup1->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    dup1->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    dup1->SetMetadataItem(GDAL_DMD_EXTENSIONS, "dupext");
    dup1->SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, "DUPPRE:");
    auto dup2 = std::make_unique<GDALDriver>();
    dup2->SetDescription("DUP_VEC2");
    dup2->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    dup2->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    dup2->SetMetadataItem(GDAL_DMD_EXTENSIONS, "dupext");
    dup2->SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, "DUPPRE:");
    GetGDALDriverManager()->RegisterDriver(dup1.get());
    GetGDALDriverManager()->RegisterDriver(dup2.get());
    absl::Cleanup deregister_dups = [&] {
      GetGDALDriverManager()->DeregisterDriver(dup2.get());
      GetGDALDriverManager()->DeregisterDriver(dup1.get());
    };
    {
      WithQuietHandler quiet;
      StringListPtr dup_ext_drvs(
          GDALGetOutputDriversForDatasetName(
              "/vsimem/gdaldriver_test/out.dupext", GDAL_OF_VECTOR,
              /*bSingleMatch=*/true, /*bEmitWarning=*/true),
          CSLDestroy);
      EXPECT_THAT(CslToVector(dup_ext_drvs.get()), ElementsAre("DUP_VEC1"));
      StringListPtr dup_pre_drvs(
          GDALGetOutputDriversForDatasetName("DUPPRE:foo", GDAL_OF_VECTOR,
                                             /*bSingleMatch=*/true,
                                             /*bEmitWarning=*/true),
          CSLDestroy);
      EXPECT_THAT(CslToVector(dup_pre_drvs.get()), ElementsAre("DUP_VEC1"));
      CPLErrorReset();
    }
  }

  StringListPtr tif_drvs(GDALGetOutputDriversForDatasetName(
                             "/vsimem/gdaldriver_test/out.tif", GDAL_OF_RASTER,
                             /*bSingleMatch=*/true,
                             /*bEmitWarning=*/true),
                         CSLDestroy);
  EXPECT_THAT(CslToVector(tif_drvs.get()), ElementsAre("GTiff"));

  StringListPtr noext_drvs(
      GDALGetOutputDriversForDatasetName("/vsimem/gdaldriver_test/out_no_ext",
                                         GDAL_OF_RASTER,
                                         /*bSingleMatch=*/true,
                                         /*bEmitWarning=*/true),
      CSLDestroy);
  EXPECT_THAT(CslToVector(noext_drvs.get()), ElementsAre("GTiff"));

  StringListPtr shp_zip_drvs(
      GDALGetOutputDriversForDatasetName("/vsimem/gdaldriver_test/out.shp.zip",
                                         GDAL_OF_VECTOR,
                                         /*bSingleMatch=*/false,
                                         /*bEmitWarning=*/false),
      CSLDestroy);

  StringListPtr gpkg_zip_drvs(
      GDALGetOutputDriversForDatasetName("/vsimem/gdaldriver_test/out.gpkg.zip",
                                         GDAL_OF_VECTOR,
                                         /*bSingleMatch=*/false,
                                         /*bEmitWarning=*/false),
      CSLDestroy);

  StringListPtr gdalg_drvs(
      GDALGetOutputDriversForDatasetName(
          "/vsimem/gdaldriver_test/out.gdalg.json", GDAL_OF_RASTER,
          /*bSingleMatch=*/true, /*bEmitWarning=*/true),
      CSLDestroy);
  EXPECT_EQ(nullptr, gdalg_drvs.get());

  // Missing plugin driver message via a dummy driver with
  // MISSING_PLUGIN_FILENAME.
  auto missing_drv = std::make_unique<GDALDriver>();
  missing_drv->SetDescription("MISSING_FOO");
  missing_drv->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  missing_drv->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
  missing_drv->SetMetadataItem(GDAL_DMD_EXTENSIONS, "missingfoo");
  missing_drv->SetMetadataItem("MISSING_PLUGIN_FILENAME", "gdal_MISSING.so");
  missing_drv->SetMetadataItem(GDAL_DMD_PLUGIN_INSTALLATION_MESSAGE,
                               "Install foo-pkg.");
  GetGDALDriverManager()->RegisterDriver(missing_drv.get());
  absl::Cleanup deregister_missing_drv = [&] {
    GetGDALDriverManager()->DeregisterDriver(missing_drv.get());
  };

  {
    WithQuietHandler quiet;
    StringListPtr missing_drvs(
        GDALGetOutputDriversForDatasetName(
            "/vsimem/gdaldriver_test/test.missingfoo", GDAL_OF_RASTER,
            /*bSingleMatch=*/true, /*bEmitWarning=*/true),
        CSLDestroy);
    EXPECT_THAT(CslToVector(missing_drvs.get()), IsEmpty());

    CPLConfigOptionSetter driver_path_setter(
        "GDAL_DRIVER_PATH", "/vsimem/gdaldriver_test/nonexistent_plugin_dir",
        /*bSetOnlyIfUndefined=*/false);
    StringListPtr missing_drvs_with_path(
        GDALGetOutputDriversForDatasetName(
            "/vsimem/gdaldriver_test/test.missingfoo", GDAL_OF_RASTER,
            /*bSingleMatch=*/true, /*bEmitWarning=*/true),
        CSLDestroy);
    EXPECT_THAT(CslToVector(missing_drvs_with_path.get()), IsEmpty());
    CPLErrorReset();
  }

  // GDALDeleteDataset and GDALRenameDataset with hDriver == nullptr on
  // unidentifiable file.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        CE_Failure,
        GDALDeleteDataset(
            nullptr, "/vsimem/gdaldriver_test/no_such_identifiable_file.xyz"));
    EXPECT_EQ(CE_Failure,
              GDALRenameDataset(
                  nullptr, "/vsimem/gdaldriver_test/new.xyz",
                  "/vsimem/gdaldriver_test/no_such_identifiable_file.xyz"));
    CPLErrorReset();
  }

  DummyDataset del_ds(1, 1, 1);
  del_ds.SetDescription("/vsimem/gdaldriver_test/del_ds_cb.bin");
  custom_drv->pfnDelete = [](const char*) -> CPLErr { return CE_None; };
  EXPECT_EQ(CE_None, custom_drv->Delete(&del_ds, nullptr));
  custom_drv->pfnDelete = nullptr;
}

}  // namespace
}  // namespace autotest2
