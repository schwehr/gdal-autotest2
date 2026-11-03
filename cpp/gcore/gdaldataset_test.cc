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
// Tests GDALDataset and related dataset APIs (gcore/gdaldataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaldataset.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdal_rasterblock.h"
#include "third_party/gdal/gcore/gdal_relationship.h"
#include "third_party/gdal/gcore/gdalantirecursion.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_featurestyle.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogr_attrind.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_multiproc.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTempDir[] = "/vsimem/gdaldataset_test";

class CustomBand : public GDALRasterBand {
 public:
  CustomBand(GDALDataset* ds, int band_num, GDALDataType dt = GDT_Byte,
             int block_x = 0, int block_y = 0) {
    poDS = ds;
    nBand = band_num;
    eDataType = dt;
    nRasterXSize = ds->GetRasterXSize();
    nRasterYSize = ds->GetRasterYSize();
    nBlockXSize = block_x > 0 ? block_x : nRasterXSize;
    nBlockYSize = block_y > 0 ? block_y : 1;
    eAccess = ds->GetAccess();
  }

  CPLErr IReadBlock(int /*x*/, int /*y*/, void* buf) override {
    const size_t bytes =
        static_cast<size_t>(GDALGetDataTypeSizeBytes(eDataType)) * nBlockXSize *
        nBlockYSize;
    std::memset(buf, nBand, bytes);
    return read_err_;
  }

  CPLErr IWriteBlock(int /*x*/, int /*y*/, void* /*buf*/) override {
    return write_err_;
  }

  CPLErr FlushCache(bool bAtClosing) override {
    if (flush_fail_) {
      return CE_Failure;
    }
    return GDALRasterBand::FlushCache(bAtClosing);
  }

  CPLErr DropCache() override {
    if (drop_fail_) {
      return CE_Failure;
    }
    return GDALRasterBand::DropCache();
  }

  CPLErr AdviseRead(int /*xoff*/, int /*yoff*/, int /*xsize*/, int /*ysize*/,
                    int /*bxsize*/, int /*bysize*/, GDALDataType /*dt*/,
                    char** /*opts*/) override {
    return advise_err_;
  }

  int GetOverviewCount() override { return ovr_count_; }

  GDALRasterBand* GetOverview(int i) override {
    if (i >= 0 && i < static_cast<int>(overviews_.size())) {
      return overviews_[i].get();
    }
    return nullptr;
  }

  GDALColorTable* GetColorTable() override { return color_table_.get(); }

  int GetMaskFlags() override {
    if (custom_mask_flags_ >= 0) {
      return custom_mask_flags_;
    }
    return GDALRasterBand::GetMaskFlags();
  }

  GDALRasterBand* GetMaskBand() override {
    if (custom_mask_band_ != nullptr) {
      return custom_mask_band_;
    }
    return GDALRasterBand::GetMaskBand();
  }

  const char* GetUnitType() override { return unit_.c_str(); }

  double GetNoDataValue(int* pbSuccess) override {
    if (pbSuccess != nullptr) {
      *pbSuccess = has_nodata_ ? 1 : 0;
    }
    return nodata_val_;
  }

  int64_t GetNoDataValueAsInt64(int* pbSuccess) override {
    if (pbSuccess != nullptr) {
      *pbSuccess = has_nodata_ ? 1 : 0;
    }
    return static_cast<int64_t>(nodata_val_);
  }

  uint64_t GetNoDataValueAsUInt64(int* pbSuccess) override {
    if (pbSuccess != nullptr) {
      *pbSuccess = has_nodata_ ? 1 : 0;
    }
    return static_cast<uint64_t>(nodata_val_);
  }

  double GetOffset(int* pbSuccess) override {
    if (pbSuccess != nullptr) {
      *pbSuccess = has_offset_ ? 1 : 0;
    }
    return offset_val_;
  }

  double GetScale(int* pbSuccess) override {
    if (pbSuccess != nullptr) {
      *pbSuccess = has_scale_ ? 1 : 0;
    }
    return scale_val_;
  }

  GDALColorInterp GetColorInterpretation() override { return color_interp_; }

  using GDALRasterBand::eAccess;
  using GDALRasterBand::nRasterXSize;
  using GDALRasterBand::nRasterYSize;

  CPLErr read_err_ = CE_None;
  CPLErr write_err_ = CE_None;
  CPLErr advise_err_ = CE_None;
  bool flush_fail_ = false;
  bool drop_fail_ = false;
  int ovr_count_ = 0;
  std::vector<std::unique_ptr<CustomBand>> overviews_;
  std::unique_ptr<GDALColorTable> color_table_;
  int custom_mask_flags_ = -1;
  GDALRasterBand* custom_mask_band_ = nullptr;
  std::string unit_;
  bool has_nodata_ = false;
  double nodata_val_ = 0.0;
  bool has_offset_ = false;
  double offset_val_ = 0.0;
  bool has_scale_ = false;
  double scale_val_ = 1.0;
  GDALColorInterp color_interp_ = GCI_Undefined;
};

class DummyAttrIndex : public OGRAttrIndex {
 public:
  GIntBig GetFirstMatch(OGRField* /*psKey*/) override { return -1; }
  GIntBig* GetAllMatches(OGRField* /*psKey*/) override { return nullptr; }
  GIntBig* GetAllMatches(OGRField* /*psKey*/, GIntBig* /*panFIDList*/,
                         int* /*nFIDCount*/, int* /*nLength*/) override {
    return nullptr;
  }
  OGRErr AddEntry(OGRField* /*psKey*/, GIntBig /*nFID*/) override {
    return OGRERR_NONE;
  }
  OGRErr RemoveEntry(OGRField* /*psKey*/, GIntBig /*nFID*/) override {
    return OGRERR_NONE;
  }
  OGRErr Clear() override { return OGRERR_NONE; }
};

class DummyLayerAttrIndex : public OGRLayerAttrIndex {
 public:
  OGRErr Initialize(const char* /*pszIndexPath*/, OGRLayer* /*lyr*/) override {
    return OGRERR_NONE;
  }
  OGRErr CreateIndex(int /*iField*/) override { return create_err_; }
  OGRErr DropIndex(int /*iField*/) override { return drop_err_; }
  OGRErr IndexAllFeatures(int /*iField*/) override { return index_err_; }
  OGRErr AddToIndex(OGRFeature* /*poFeature*/, int /*iField*/) override {
    return OGRERR_NONE;
  }
  OGRErr RemoveFromIndex(OGRFeature* /*poFeature*/) override {
    return OGRERR_NONE;
  }
  OGRAttrIndex* GetFieldIndex(int /*iField*/) override {
    return has_field_index_ ? &attr_idx_ : nullptr;
  }

  OGRErr create_err_ = OGRERR_NONE;
  OGRErr drop_err_ = OGRERR_NONE;
  OGRErr index_err_ = OGRERR_NONE;
  bool has_field_index_ = true;
  DummyAttrIndex attr_idx_;
};

class CustomLayer : public OGRLayer {
 public:
  explicit CustomLayer(const char* name, OGRwkbGeometryType gtype = wkbPoint,
                       const OGRSpatialReference* srs = nullptr) {
    SetDescription(name);
    defn_ = new OGRFeatureDefn(name);
    defn_->Reference();
    defn_->SetGeomType(gtype);
    if (gtype != wkbNone && defn_->GetGeomFieldCount() > 0 && srs != nullptr) {
      OGRSpatialReference* cloned_srs = srs->Clone();
      defn_->GetGeomFieldDefn(0)->SetSpatialRef(cloned_srs);
      cloned_srs->Release();
    }
    extent_.MinX = 0.0;
    extent_.MaxX = 10.0;
    extent_.MinY = 0.0;
    extent_.MaxY = 10.0;
  }

  ~CustomLayer() override { defn_->Release(); }

  void EnableDummyIndex() {
    if (m_poAttrIndex == nullptr) {
      // Owned by OGRLayer::~OGRLayer().
      dummy_idx_ = new DummyLayerAttrIndex();
      m_poAttrIndex = dummy_idx_;
    }
  }

  void ResetReading() override { read_idx_ = 0; }

  OGRFeature* GetNextFeature() override {
    if (read_idx_ < features_.size()) {
      return features_[read_idx_++]->Clone();
    }
    return nullptr;
  }

  const OGRFeatureDefn* GetLayerDefn() const override {
    if (null_defn_before_field_ && defn_->GetFieldCount() == 0) {
      return nullptr;
    }
    return defn_;
  }

  int TestCapability(const char* cap) const override {
    if (EQUAL(cap, OLCFastFeatureCount)) {
      return fast_feature_count_ ? 1 : 0;
    }
    if (EQUAL(cap, OLCFastGetExtent)) {
      return fast_get_extent_ ? 1 : 0;
    }
    if (EQUAL(cap, OLCTransactions)) {
      return support_transactions_ ? 1 : 0;
    }
    if (EQUAL(cap, OLCCurveGeometries)) {
      return support_curves_ ? 1 : 0;
    }
    return 0;
  }

  GIntBig GetFeatureCount(int /*bForce*/) override {
    if (neg_feature_count_) {
      return -1;
    }
    return static_cast<GIntBig>(features_.size());
  }

  OGRErr IGetExtent(int /*iGeomField*/, OGREnvelope* psExtent,
                    bool /*bForce*/) override {
    if (!has_extent_) {
      return OGRERR_FAILURE;
    }
    *psExtent = extent_;
    return OGRERR_NONE;
  }

  OGRErr SyncToDisk() override { return sync_err_; }

  OGRErr CreateField(const OGRFieldDefn* field, int /*bApproxOK*/) override {
    if (create_field_err_ != OGRERR_NONE) {
      return create_field_err_;
    }
    if (!skip_adding_field_on_create_) {
      defn_->AddFieldDefn(field);
    }
    return OGRERR_NONE;
  }

  OGRErr DeleteField(int iField) override {
    return defn_->DeleteFieldDefn(iField);
  }

  OGRErr AlterFieldDefn(int iField, OGRFieldDefn* poNewFieldDefn,
                        int nFlagsIn) override {
    OGRFieldDefn* f = defn_->GetFieldDefn(iField);
    if (f == nullptr) {
      return OGRERR_FAILURE;
    }
    if (nFlagsIn & ALTER_NAME_FLAG) {
      f->SetName(poNewFieldDefn->GetNameRef());
    }
    if (nFlagsIn & ALTER_TYPE_FLAG) {
      f->SetType(poNewFieldDefn->GetType());
    }
    if (nFlagsIn & ALTER_WIDTH_PRECISION_FLAG) {
      f->SetWidth(poNewFieldDefn->GetWidth());
      f->SetPrecision(poNewFieldDefn->GetPrecision());
    }
    return OGRERR_NONE;
  }

  OGRErr CreateGeomField(const OGRGeomFieldDefn* field,
                         int /*bApproxOK*/) override {
    OGRGeomFieldDefn copy(field);
    if (copy.GetSpatialRef() != nullptr) {
      OGRSpatialReference* cloned = copy.GetSpatialRef()->Clone();
      copy.SetSpatialRef(cloned);
      cloned->Release();
    }
    defn_->AddGeomFieldDefn(&copy);
    return OGRERR_NONE;
  }

  OGRErr Rename(const char* new_name) override {
    SetDescription(new_name);
    defn_->SetName(new_name);
    return OGRERR_NONE;
  }

  OGRErr ICreateFeature(OGRFeature* feat) override {
    if (create_feature_err_ != OGRERR_NONE) {
      return create_feature_err_;
    }
    features_.emplace_back(feat->Clone());
    return OGRERR_NONE;
  }

  OGRErr StartTransaction() override { return start_tx_err_; }
  OGRErr CommitTransaction() override { return commit_tx_err_; }
  OGRErr RollbackTransaction() override {
    rollback_called_ = true;
    start_tx_err_ = OGRERR_FAILURE;
    return OGRERR_NONE;
  }

  const char* GetFIDColumn() const override { return fid_col_.c_str(); }

  OGRFeatureDefn* defn_ = nullptr;
  DummyLayerAttrIndex* dummy_idx_ = nullptr;
  std::vector<OGRFeatureUniquePtr> features_;
  size_t read_idx_ = 0;
  bool fast_feature_count_ = true;
  bool neg_feature_count_ = false;
  bool fast_get_extent_ = true;
  bool has_extent_ = true;
  OGREnvelope extent_;
  bool support_transactions_ = false;
  bool support_curves_ = false;
  bool null_defn_before_field_ = false;
  bool skip_adding_field_on_create_ = false;
  OGRErr sync_err_ = OGRERR_NONE;
  OGRErr create_field_err_ = OGRERR_NONE;
  OGRErr create_feature_err_ = OGRERR_NONE;
  OGRErr start_tx_err_ = OGRERR_NONE;
  OGRErr commit_tx_err_ = OGRERR_NONE;
  bool rollback_called_ = false;
  std::string fid_col_;
};

class DummyGroup : public GDALGroup {
 public:
  DummyGroup() : GDALGroup(std::string(), "/") {}
  void ClearStatistics() override { cleared_ = true; }
  bool cleared_ = false;
};

class ExposedDataset : public GDALDataset {
 public:
  ExposedDataset() = default;
  explicit ExposedDataset(int force_cached_io) : GDALDataset(force_cached_io) {}
  ~ExposedDataset() override = default;

  using GDALDataset::AcquireMutex;
  using GDALDataset::BandBasedRasterIO;
  using GDALDataset::bForceCachedIO;
  using GDALDataset::bIsInternal;
  using GDALDataset::BlockBasedFlushCache;
  using GDALDataset::bShared;
  using GDALDataset::BuildParseInfo;
  using GDALDataset::CanBeCloned;
  using GDALDataset::CleanupPostFileClosing;
  using GDALDataset::Clone;
  using GDALDataset::CloseDependentDatasets;
  using GDALDataset::DestroyParseInfo;
  using GDALDataset::DisableReadWriteMutex;
  using GDALDataset::eAccess;
  using GDALDataset::EnterReadWrite;
  using GDALDataset::GetGeoTransform;
  using GDALDataset::IBuildOverviews;
  using GDALDataset::ICreateLayer;
  using GDALDataset::InitRWLock;
  using GDALDataset::IRasterIO;
  using GDALDataset::IsAllBands;
  using GDALDataset::LeaveReadWrite;
  using GDALDataset::m_bCanBeReopened;
  using GDALDataset::m_oMapFieldDomains;
  using GDALDataset::m_poStyleTable;
  using GDALDataset::nBands;
  using GDALDataset::nOpenFlags;
  using GDALDataset::nRasterXSize;
  using GDALDataset::nRasterYSize;
  using GDALDataset::oOvManager;
  using GDALDataset::papoBands;
  using GDALDataset::papszOpenOptions;
  using GDALDataset::poDriver;
  using GDALDataset::ProcessSQLAlterTableAddColumn;
  using GDALDataset::ProcessSQLAlterTableAlterColumn;
  using GDALDataset::ProcessSQLAlterTableDropColumn;
  using GDALDataset::ProcessSQLAlterTableRenameColumn;
  using GDALDataset::ProcessSQLCreateIndex;
  using GDALDataset::ProcessSQLDropIndex;
  using GDALDataset::ProcessSQLDropTable;
  using GDALDataset::RasterInitialize;
  using GDALDataset::ReacquireReadWriteLock;
  using GDALDataset::ReleaseMutex;
  using GDALDataset::SetBand;
  using GDALDataset::ShareLockWithParentDataset;
  using GDALDataset::TemporarilyDropReadWriteLock;
  using GDALDataset::ValidateLayerCreationOptions;
  using GDALDataset::ValidateRasterIOOrAdviseReadParameters;

  const OGRSpatialReference* GetSpatialRef() const override {
    if (use_custom_srs_) {
      return custom_srs_.get();
    }
    return GDALDataset::GetSpatialRef();
  }

  const OGRSpatialReference* GetGCPSpatialRef() const override {
    if (use_custom_gcp_srs_) {
      return custom_gcp_srs_.get();
    }
    return GDALDataset::GetGCPSpatialRef();
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    if (has_custom_gt_) {
      gt = custom_gt_;
      return CE_None;
    }
    return GDALDataset::GetGeoTransform(gt);
  }

  int GetLayerCount() const override {
    return static_cast<int>(layers_.size());
  }

  const OGRLayer* GetLayer(int i) const override {
    if (i >= 0 && i < static_cast<int>(layers_.size())) {
      return layers_[i].get();
    }
    return nullptr;
  }

  OGRErr DeleteLayer(int i) override {
    if (!allow_delete_layer_) {
      return GDALDataset::DeleteLayer(i);
    }
    if (i >= 0 && i < static_cast<int>(layers_.size())) {
      layers_.erase(layers_.begin() + i);
      return OGRERR_NONE;
    }
    return OGRERR_FAILURE;
  }

  OGRLayer* ICreateLayer(const char* name,
                         const OGRGeomFieldDefn* geom_field_defn,
                         CSLConstList /*options*/) override {
    if (!allow_create_layer_) {
      return GDALDataset::ICreateLayer(name, geom_field_defn, nullptr);
    }
    const OGRwkbGeometryType gtype =
        geom_field_defn != nullptr ? geom_field_defn->GetType() : wkbNone;
    const OGRSpatialReference* srs =
        geom_field_defn != nullptr ? geom_field_defn->GetSpatialRef() : nullptr;
    auto lyr = std::make_unique<CustomLayer>(name, gtype, srs);
    lyr->support_transactions_ = created_layers_support_tx_;
    lyr->null_defn_before_field_ = created_layers_null_defn_before_field_;
    lyr->skip_adding_field_on_create_ = created_layers_skip_adding_field_;
    lyr->create_feature_err_ = created_layers_create_feature_err_;
    lyr->start_tx_err_ = created_layers_start_tx_err_;
    lyr->commit_tx_err_ = created_layers_commit_tx_err_;
    for (const auto& pre_field : precreated_fields_) {
      lyr->defn_->AddFieldDefn(&pre_field);
    }
    CustomLayer* ptr = lyr.get();
    layers_.push_back(std::move(lyr));
    return ptr;
  }

  int TestCapability(const char* cap) const override {
    if (EQUAL(cap, ODsCCreateLayer)) {
      return allow_create_layer_ ? 1 : 0;
    }
    if (EQUAL(cap, ODsCDeleteLayer)) {
      return allow_delete_layer_ ? 1 : 0;
    }
    if (EQUAL(cap, ODsCCreateGeomFieldAfterCreateLayer)) {
      return allow_create_geom_field_ ? 1 : 0;
    }
    if (EQUAL(cap, ODsCCurveGeometries)) {
      return allow_curves_ ? 1 : 0;
    }
    return GDALDataset::TestCapability(cap);
  }

  std::shared_ptr<GDALGroup> GetRootGroup() const override {
    if (root_group_ != nullptr) {
      return root_group_;
    }
    return GDALDataset::GetRootGroup();
  }

  bool use_custom_srs_ = false;
  std::unique_ptr<OGRSpatialReference> custom_srs_;
  bool use_custom_gcp_srs_ = false;
  std::unique_ptr<OGRSpatialReference> custom_gcp_srs_;
  bool has_custom_gt_ = false;
  GDALGeoTransform custom_gt_;
  std::vector<std::unique_ptr<CustomLayer>> layers_;
  bool allow_create_layer_ = false;
  bool allow_delete_layer_ = false;
  bool allow_create_geom_field_ = false;
  bool allow_curves_ = false;
  bool created_layers_support_tx_ = false;
  bool created_layers_null_defn_before_field_ = false;
  bool created_layers_skip_adding_field_ = false;
  OGRErr created_layers_create_feature_err_ = OGRERR_NONE;
  OGRErr created_layers_start_tx_err_ = OGRERR_NONE;
  OGRErr created_layers_commit_tx_err_ = OGRERR_NONE;
  std::vector<OGRFieldDefn> precreated_fields_;
  std::shared_ptr<DummyGroup> root_group_;
};

using ExposedDatasetPtr =
    std::unique_ptr<ExposedDataset, GDALDatasetUniquePtrDeleter>;

// Writes a file to /vsimem/ when the test expects GDAL itself to unlink it
// (for example via MarkSuppressOnClose()).
void WriteVsiMemFileForSuppressOnClose(const char* path,
                                       std::string_view data = "x") {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));
  ASSERT_EQ(0, VSIFCloseL(fp));
}

TEST(GdalDatasetTest, LifecycleResponsiblePidCloseAndSuppressOnClose) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTempDir); };

  EXPECT_NE(nullptr, GDALGetphDLMutex());
  const GIntBig orig_pid = CPLGetPID();
  absl::Cleanup restore_pid = [orig_pid] {
    GDALSetResponsiblePIDForCurrentThread(orig_pid);
  };
  EXPECT_EQ(orig_pid, GDALGetResponsiblePIDForCurrentThread());
  GDALSetResponsiblePIDForCurrentThread(orig_pid + 42);
  EXPECT_EQ(orig_pid + 42, GDALGetResponsiblePIDForCurrentThread());
  GDALSetResponsiblePIDForCurrentThread(orig_pid);

  // 1. Test GDALDataset(int) constructor and Close / SuppressOnClose.
  constexpr char kSuppressFile[] = "/vsimem/gdaldataset_test/suppress_test.bin";
  WriteVsiMemFileForSuppressOnClose(kSuppressFile);
  {
    ExposedDataset ds(1);
    EXPECT_TRUE(ds.bForceCachedIO);
    ds.SetDescription(kSuppressFile);
    EXPECT_FALSE(ds.IsMarkedSuppressOnClose());
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetMarkSuppressOnClose(nullptr);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    GDALDatasetMarkSuppressOnClose(GDALDataset::ToHandle(&ds));
    EXPECT_TRUE(ds.IsMarkedSuppressOnClose());
    ds.UnMarkSuppressOnClose();
    EXPECT_FALSE(ds.IsMarkedSuppressOnClose());
    ds.MarkSuppressOnClose();
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, GDALDatasetRunCloseWithoutDestroying(nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    EXPECT_EQ(CE_None,
              GDALDatasetRunCloseWithoutDestroying(GDALDataset::ToHandle(&ds)));
    // File was unlinked and suppress flag cleared.
    EXPECT_FALSE(ds.IsMarkedSuppressOnClose());
    VSIStatBufL stat_buf = {};
    EXPECT_NE(0, VSIStatL(kSuppressFile, &stat_buf));
  }

  // 2. Test CleanupPostFileClosing.
  constexpr char kCleanupFile[] = "/vsimem/gdaldataset_test/cleanup_post.bin";
  WriteVsiMemFileForSuppressOnClose(kCleanupFile);
  {
    ExposedDataset ds;
    ds.SetDescription(kCleanupFile);
    ds.MarkSuppressOnClose();
    ds.CleanupPostFileClosing();
    VSIStatBufL stat_buf = {};
    EXPECT_NE(0, VSIStatL(kCleanupFile, &stat_buf));
  }

  // 3. Test destructor with responsiblePID != CPLGetPID() on non-internal
  // dataset.
  constexpr char kTifPath[] = "/vsimem/gdaldataset_test/resp_pid.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr tif_ds(
      gtiff_drv->Create(kTifPath, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, tif_ds);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALSetResponsiblePIDForCurrentThread(orig_pid + 99);
    tif_ds.reset();
    GDALSetResponsiblePIDForCurrentThread(orig_pid);
    CPLErrorReset();
  }
}

TEST(GdalDatasetTest, FlushCacheDropCacheAndBlockBasedFlushCache) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALFlushCache(nullptr));
    EXPECT_EQ(CE_Failure, GDALDropCache(nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  ds.RasterInitialize(8, 8);
  EXPECT_EQ(-1, ds.GetEstimatedRAMUsage());

  // Empty dataset BlockBasedFlushCache.
  EXPECT_EQ(CE_None, ds.BlockBasedFlushCache(false));

  auto* b1 = new CustomBand(&ds, 1, GDT_Byte, 4, 4);
  auto b2_owner = std::make_unique<CustomBand>(&ds, 2, GDT_Byte, 4, 4);
  CustomBand* b2 = b2_owner.get();
  ds.SetBand(1, b1);
  ds.SetBand(2, std::move(b2_owner));

  auto lyr = std::make_unique<CustomLayer>("l1");
  CustomLayer* lyr_ptr = lyr.get();
  ds.layers_.push_back(std::move(lyr));

  EXPECT_EQ(CE_None, GDALFlushCache(GDALDataset::ToHandle(&ds)));
  EXPECT_EQ(CE_None, GDALDropCache(GDALDataset::ToHandle(&ds)));

  // Failing band FlushCache / DropCache and layer SyncToDisk.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    b1->flush_fail_ = true;
    lyr_ptr->sync_err_ = OGRERR_FAILURE;
    EXPECT_EQ(CE_Failure, ds.FlushCache(false));
    b1->flush_fail_ = false;
    lyr_ptr->sync_err_ = OGRERR_NONE;

    b2->drop_fail_ = true;
    EXPECT_EQ(CE_Failure, ds.DropCache());
    b2->drop_fail_ = false;
    CPLErrorReset();
  }

  // BlockBasedFlushCache with matching block sizes.
  EXPECT_EQ(CE_None, ds.BlockBasedFlushCache(false));

  // Dirty a block in b1 and make IWriteBlock fail during BlockBasedFlushCache.
  ds.eAccess = GA_Update;
  b1->eAccess = GA_Update;
  GDALRasterBlock* blk = b1->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(nullptr, blk);
  blk->MarkDirty();
  blk->DropLock();
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    b1->write_err_ = CE_Failure;
    EXPECT_EQ(CE_Failure, ds.BlockBasedFlushCache(false));
    b1->write_err_ = CE_None;
    CPLErrorReset();
  }

  // BlockBasedFlushCache when marked suppress on close & bAtClosing == true.
  ds.MarkSuppressOnClose();
  EXPECT_EQ(CE_None, ds.BlockBasedFlushCache(true));
  ds.UnMarkSuppressOnClose();

  // BlockBasedFlushCache when band 3 has different block X and Y sizes.
  auto* b3 = new CustomBand(&ds, 3, GDT_Byte, 2, 2);
  ds.SetBand(3, b3);
  EXPECT_EQ(CE_None, ds.BlockBasedFlushCache(false));
}

TEST(GdalDatasetTest, BandsRasterSizeAndSetBandEdgeCases) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, GDALGetRasterXSize(nullptr));
    EXPECT_EQ(0, GDALGetRasterYSize(nullptr));
    EXPECT_EQ(0, GDALGetRasterCount(nullptr));
    EXPECT_EQ(nullptr, GDALGetRasterBand(nullptr, 1));
    EXPECT_EQ(CE_Failure, GDALAddBand(nullptr, GDT_Byte, nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  ds.RasterInitialize(16, 12);
  const ExposedDataset& const_ds = ds;

  EXPECT_EQ(16, GDALGetRasterXSize(GDALDataset::ToHandle(&ds)));
  EXPECT_EQ(12, GDALGetRasterYSize(GDALDataset::ToHandle(&ds)));
  EXPECT_EQ(0, GDALGetRasterCount(GDALDataset::ToHandle(&ds)));
  EXPECT_EQ(nullptr, ds.GetRasterBand(1));
  EXPECT_EQ(nullptr, const_ds.GetRasterBand(1));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALAddBand(GDALDataset::ToHandle(&ds), GDT_Byte, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // SetBand skipping band 1 (sets band 2, leaving band 1 as nullptr).
  auto* b2 = new CustomBand(&ds, 2);
  ds.SetBand(2, b2);
  EXPECT_EQ(2, ds.GetRasterCount());
  EXPECT_EQ(nullptr, ds.GetRasterBand(1));
  EXPECT_EQ(b2, ds.GetRasterBand(2));
  EXPECT_EQ(b2, const_ds.GetRasterBand(2));

  // Out of bounds GetRasterBand (const and non-const).
  EXPECT_EQ(nullptr, ds.GetRasterBand(0));
  EXPECT_EQ(nullptr, ds.GetRasterBand(3));
  EXPECT_EQ(nullptr, const_ds.GetRasterBand(0));
  EXPECT_EQ(nullptr, const_ds.GetRasterBand(3));

  // Setting already-set band 2 fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CustomBand duplicate_band(&ds, 2);
    ds.SetBand(2, &duplicate_band);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  EXPECT_EQ(b2, ds.GetRasterBand(2));

  // Set band 1 properly.
  auto* b1 = new CustomBand(&ds, 1);
  ds.SetBand(1, b1);

  // Test GetBands() and ConstBands iterators and indexing.
  auto bands = ds.GetBands();
  EXPECT_EQ(2U, bands.size());
  EXPECT_EQ(b1, bands[0]);
  EXPECT_EQ(b2, bands[static_cast<size_t>(1)]);
  int count = 0;
  for (GDALRasterBand* b : bands) {
    EXPECT_NE(nullptr, b);
    ++count;
  }
  EXPECT_EQ(2, count);

  auto const_bands = const_ds.GetBands();
  EXPECT_EQ(2U, const_bands.size());
  EXPECT_EQ(b1, const_bands[0]);
  EXPECT_EQ(b2, const_bands[static_cast<size_t>(1)]);
  count = 0;
  for (const GDALRasterBand* b : const_bands) {
    EXPECT_NE(nullptr, b);
    ++count;
  }
  EXPECT_EQ(2, count);
}

TEST(GdalDatasetTest, SpatialRefGeoTransformAndGCPs) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALGetSpatialRef(nullptr));
    EXPECT_EQ(nullptr, GDALGetProjectionRef(nullptr));
    EXPECT_EQ(CE_Failure, GDALSetSpatialRef(nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetProjection(nullptr, "EPSG:4326"));
    EXPECT_EQ(CE_Failure, GDALGetGeoTransform(nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetGeoTransform(nullptr, nullptr));
    EXPECT_EQ(0, GDALGetGCPCount(nullptr));
    EXPECT_EQ(nullptr, GDALGetGCPSpatialRef(nullptr));
    EXPECT_EQ(nullptr, GDALGetGCPProjection(nullptr));
    EXPECT_EQ(nullptr, GDALGetGCPs(nullptr));
    EXPECT_EQ(CE_Failure, GDALSetGCPs(nullptr, 0, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetGCPs2(nullptr, 0, nullptr, nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // 1. Default SpatialRef / ProjectionRef.
  EXPECT_EQ(nullptr, GDALGetSpatialRef(hDS));
  EXPECT_STREQ("", GDALGetProjectionRef(hDS));
  EXPECT_EQ(nullptr, ds.GetSpatialRefRasterOnly());

  // 2. SetProjection / SetSpatialRef on base class.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSetProjection(hDS, ""));
    EXPECT_EQ(CE_Failure, GDALSetProjection(hDS, "INVALID_SRS_STRING"));
    EXPECT_EQ(CE_Failure, GDALSetProjection(hDS, "EPSG:4326"));
    ds.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALSetSpatialRef(hDS, nullptr));
    ds.SetMOFlags(0);
    CPLErrorReset();
  }

  // 3. Custom SRS caching in GetProjectionRef.
  ds.use_custom_srs_ = true;
  ds.custom_srs_ = std::make_unique<OGRSpatialReference>();
  ds.custom_srs_->SetWellKnownGeogCS("WGS84");
  const char* wkt1 = ds.GetProjectionRef();
  ASSERT_NE(nullptr, wkt1);
  EXPECT_STRNE("", wkt1);
  const char* wkt2 = ds.GetProjectionRef();
  EXPECT_EQ(wkt1, wkt2);  // Cached pointer returned.
  ds.custom_srs_->SetWellKnownGeogCS("NAD27");
  const char* wkt3 = ds.GetProjectionRef();
  ASSERT_NE(nullptr, wkt3);
  EXPECT_STRNE("", wkt3);
  ds.use_custom_srs_ = false;

  // 4. GetSpatialRefVectorOnly with layers.
  OGRSpatialReference srs1;
  srs1.SetWellKnownGeogCS("WGS84");
  OGRSpatialReference srs2;
  srs2.SetWellKnownGeogCS("NAD27");

  ds.layers_.push_back(std::make_unique<CustomLayer>("l1", wkbPoint, &srs1));
  EXPECT_NE(nullptr, ds.GetSpatialRef());
  ds.layers_.push_back(std::make_unique<CustomLayer>("l2", wkbPoint, &srs1));
  EXPECT_NE(nullptr, ds.GetSpatialRef());
  // Layer with different SRS -> returns nullptr.
  ds.layers_.push_back(std::make_unique<CustomLayer>("l3", wkbPoint, &srs2));
  EXPECT_EQ(nullptr, ds.GetSpatialRef());
  ds.layers_.clear();

  // 5. GeoTransform.
  double adfGT[6] = {0.0};
  EXPECT_EQ(CE_Failure, ds.GetGeoTransform(adfGT));
  EXPECT_EQ(CE_Failure, GDALGetGeoTransform(hDS, adfGT));
  EXPECT_DOUBLE_EQ(1.0, adfGT[1]);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSetGeoTransform(hDS, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetGeoTransform(hDS, adfGT));
    EXPECT_EQ(CE_Failure, ds.SetGeoTransform(adfGT));
    ds.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, ds.SetGeoTransform(adfGT));
    ds.SetMOFlags(0);
    CPLErrorReset();
  }

  // 6. GCPs.
  EXPECT_EQ(0, GDALGetGCPCount(hDS));
  EXPECT_EQ(nullptr, GDALGetGCPSpatialRef(hDS));
  EXPECT_STREQ("", GDALGetGCPProjection(hDS));
  EXPECT_EQ(nullptr, GDALGetGCPs(hDS));
  char* srs_wkt_raw = nullptr;
  ASSERT_EQ(OGRERR_NONE, srs1.exportToWkt(&srs_wkt_raw));
  const std::unique_ptr<char, CplFreeDeleter> srs_wkt(srs_wkt_raw);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSetGCPs(hDS, 0, nullptr, ""));
    EXPECT_EQ(CE_Failure, GDALSetGCPs(hDS, 0, nullptr, "INVALID_WKT"));
    EXPECT_EQ(CE_Failure, GDALSetGCPs(hDS, 0, nullptr, srs_wkt.get()));
    EXPECT_EQ(CE_Failure, GDALSetGCPs2(hDS, 0, nullptr,
                                       OGRSpatialReference::ToHandle(&srs1)));
    ds.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALSetGCPs2(hDS, 0, nullptr, nullptr));
    ds.SetMOFlags(0);
    CPLErrorReset();
  }

  // 7. Custom GCP SRS caching in GetGCPProjection.
  ds.use_custom_gcp_srs_ = true;
  ds.custom_gcp_srs_ = std::make_unique<OGRSpatialReference>();
  ds.custom_gcp_srs_->SetWellKnownGeogCS("WGS84");
  const char* gcp_wkt1 = ds.GetGCPProjection();
  ASSERT_NE(nullptr, gcp_wkt1);
  EXPECT_STRNE("", gcp_wkt1);
  const char* gcp_wkt2 = ds.GetGCPProjection();
  EXPECT_EQ(gcp_wkt1, gcp_wkt2);
  ds.custom_gcp_srs_->SetWellKnownGeogCS("NAD27");
  const char* gcp_wkt3 = ds.GetGCPProjection();
  ASSERT_NE(nullptr, gcp_wkt3);
  EXPECT_STRNE("", gcp_wkt3);
}

TEST(GdalDatasetTest, InternalHandleDriverRefCountsAndSharedDatasets) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTempDir); };

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALGetInternalHandle(nullptr, "foo"));
    EXPECT_EQ(nullptr, GDALGetDatasetDriver(nullptr));
    EXPECT_EQ(0, GDALReferenceDataset(nullptr));
    EXPECT_EQ(0, GDALDereferenceDataset(nullptr));
    EXPECT_EQ(0, GDALReleaseDataset(nullptr));
    EXPECT_EQ(0, GDALGetAccess(nullptr));
    GDALGetOpenDatasets(nullptr, nullptr);
    CPLErrorReset();
  }

  auto* heap_ds = new ExposedDataset();
  GDALDatasetH hDS = GDALDataset::ToHandle(heap_ds);
  EXPECT_EQ(nullptr, GDALGetInternalHandle(hDS, "foo"));
  EXPECT_EQ(nullptr, GDALGetDatasetDriver(hDS));
  EXPECT_STREQ("", heap_ds->GetDriverName());
  EXPECT_EQ(GA_ReadOnly, GDALGetAccess(hDS));

  EXPECT_EQ(1, heap_ds->GetRefCount());
  EXPECT_EQ(2, GDALReferenceDataset(hDS));
  EXPECT_EQ(2, heap_ds->GetSummaryRefCount());

  // Add a layer and check GetSummaryRefCount.
  auto lyr = std::make_unique<CustomLayer>("l1");
  lyr->Reference();
  heap_ds->layers_.push_back(std::move(lyr));
  EXPECT_EQ(3, heap_ds->GetSummaryRefCount());

  // ReleaseRef when refcount > 1 returns FALSE.
  EXPECT_EQ(FALSE, GDALReleaseDataset(hDS));
  EXPECT_EQ(1, heap_ds->GetRefCount());
  EXPECT_EQ(2, GDALReferenceDataset(hDS));
  EXPECT_EQ(1, GDALDereferenceDataset(hDS));

  // MarkAsShared on internal dataset.
  EXPECT_EQ(FALSE, heap_ds->GetShared());
  heap_ds->MarkAsShared();
  EXPECT_EQ(TRUE, heap_ds->GetShared());

  // Release() destroys when refcount drops to 0.
  EXPECT_EQ(OGRERR_NONE, heap_ds->Release());

  // Test duplicate MarkAsShared on two non-internal datasets with same name.
  constexpr char kGtiffFile1[] = "/vsimem/gdaldataset_test/shared_dup1.tif";
  constexpr char kGtiffFile2[] = "/vsimem/gdaldataset_test/shared_dup2.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr ds1(
      gtiff_drv->Create(kGtiffFile1, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds1);
  GDALDatasetUniquePtr ds2(
      gtiff_drv->Create(kGtiffFile2, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds2);
  ds1->MarkAsShared();
  ds2->SetDescription(kGtiffFile1);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ds2->MarkAsShared();  // Duplicate description in phSharedDatasetSet.
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  // Change ds1's description so UnregisterFromSharedDataset hits the 'else'
  // branch.
  ds1->SetDescription("/vsimem/gdaldataset_test/changed_desc.tif");
  ds1.reset();
  ds2->SetDescription(kGtiffFile2);
  ds2.reset();
}

TEST(GdalDatasetTest, BuildOverviewsAddOverviewsFileListAndMaskBand) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTempDir); };

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALBuildOverviews(nullptr, "NEAREST", 0, nullptr, 0,
                                             nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure,
              GDALBuildOverviewsEx(nullptr, "NEAREST", 0, nullptr, 0, nullptr,
                                   nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, GDALGetFileList(nullptr));
    EXPECT_EQ(CE_Failure, GDALCreateDatasetMaskBand(nullptr, GMF_PER_DATASET));
    CPLErrorReset();
  }

  // 1. Uninitialized oOvManager dataset.
  ExposedDataset ds;
  ds.RasterInitialize(8, 8);
  ds.SetBand(1, new CustomBand(&ds, 1));

  // Fake driver with GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST metadata.
  GDALDriver fake_drv;
  fake_drv.SetDescription("FAKE_DRV");
  fake_drv.SetMetadataItem(GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST,
                           "<OverviewCreationOptionList>"
                           "  <Option name='LOCATION' "
                           "type='string-select'><Value>RRD</Value></Option>"
                           "  <Option name='VIRTUAL' type='boolean'/>"
                           "  <Option name='COMPRESS' type='string'/>"
                           "</OverviewCreationOptionList>");
  ds.poDriver = &fake_drv;
  EXPECT_STREQ("FAKE_DRV", ds.GetDriverName());

  constexpr std::array<const char*, 4> kOvrOpts = {
      "USE_RRD=YES", "VRT_VIRTUAL_OVERVIEWS=YES", "COMPRESS_OVERVIEW=DEFLATE",
      nullptr};
  constexpr std::array<int, 1> kBadOvrList = {-2};
  constexpr std::array<int, 1> kGoodOvrList = {2};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALBuildOverviewsEx(GDALDataset::ToHandle(&ds), "NEAREST", 1,
                                   kBadOvrList.data(), 0, nullptr, nullptr,
                                   nullptr, kOvrOpts.data()));

    // Fails because oOvManager is not initialized.
    EXPECT_EQ(CE_Failure, GDALBuildOverviews(GDALDataset::ToHandle(&ds),
                                             "NEAREST", 1, kGoodOvrList.data(),
                                             0, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CE_Failure, ds.AddOverviews({}, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALCreateDatasetMaskBand(GDALDataset::ToHandle(&ds),
                                                    GMF_PER_DATASET));
    CPLErrorReset();
  }

  // 2. Real GTiff in /vsimem/ with external overview and mask.
  constexpr char kTifPath[] = "/vsimem/gdaldataset_test/ovr_mask_test.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr tif_ds(
      gtiff_drv->Create(kTifPath, 16, 16, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, tif_ds);
  tif_ds.reset();

  // Open read-only so BuildOverviews creates an external .ovr file.
  GDALDatasetUniquePtr tif_ds_ro(GDALDataset::FromHandle(GDALOpenEx(
      kTifPath, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, tif_ds_ro);
  EXPECT_EQ(CE_None,
            tif_ds_ro->BuildOverviews("NEAREST", 1, kGoodOvrList.data(), 0,
                                      nullptr, nullptr, nullptr, nullptr));
  {
    CPLConfigOptionSetter msk_setter("GDAL_TIFF_INTERNAL_MASK", "NO", false);
    EXPECT_EQ(CE_None,
              GDALCreateDatasetMaskBand(GDALDataset::ToHandle(tif_ds_ro.get()),
                                        GMF_PER_DATASET));
  }
  const StringListPtr file_list(
      GDALGetFileList(GDALDataset::ToHandle(tif_ds_ro.get())), CSLDestroy);
  EXPECT_GE(CslToVector(file_list.get()).size(), 2U);
  tif_ds_ro.reset();

  // 3. Test GDALAntiRecursionGuard.
  GDALAntiRecursionGuard g1("test_id");
  EXPECT_EQ(1, g1.GetCallDepth());
  GDALAntiRecursionGuard g2(g1, "_sub");
  EXPECT_EQ(1, g2.GetCallDepth());
  GDALAntiRecursionGuard g3(g1, "");
  EXPECT_EQ(0, g3.GetCallDepth());
}

TEST(GdalDatasetTest, RasterIOAndAdviseReadComprehensive) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(nullptr, GF_Read, 0, 0, 1, 1, nullptr, 1, 1,
                                  GDT_Byte, 1, nullptr, 0, 0, 0));
    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIOEx(nullptr, GF_Read, 0, 0, 1, 1, nullptr, 1, 1,
                                    GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, GDALDatasetAdviseRead(nullptr, 0, 0, 1, 1, 1, 1,
                                                GDT_Byte, 1, nullptr, nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  ds.RasterInitialize(8, 8);
  auto* b1 = new CustomBand(&ds, 1, GDT_Byte, 8, 1);
  auto* b2 = new CustomBand(&ds, 2, GDT_Byte, 8, 1);
  ds.SetBand(1, b1);
  ds.SetBand(2, b2);

  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);
  std::vector<GByte> buf(128, 0);

  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);

  // 1. Error cases: invalid ExtraArg version, null buffer, GF_Write on
  // read-only, window out of range, band count > GetRasterCount, invalid band
  // map, invalid eBufType.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    extra.nVersion = RASTERIO_EXTRA_ARG_CURRENT_VERSION + 10;
    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                    GDT_Byte, 1, nullptr, 0, 0, 0, &extra));
    INIT_RASTERIO_EXTRA_ARG(extra);

    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, nullptr, 4, 4,
                                  GDT_Byte, 1, nullptr, 0, 0, 0));

    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Write, 0, 0, 4, 4, buf.data(), 4, 4,
                                  GDT_Byte, 1, nullptr, 0, 0, 0));

    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Read, -1, 0, 4, 4, buf.data(), 4, 4,
                                  GDT_Byte, 1, nullptr, 0, 0, 0));
    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 10, 4, buf.data(), 4, 4,
                                  GDT_Byte, 1, nullptr, 0, 0, 0));

    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                  GDT_Byte, 5, nullptr, 0, 0, 0));

    constexpr std::array<int, 1> kBadMap = {0};
    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                  GDT_Byte, 1, const_cast<int*>(kBadMap.data()),
                                  0, 0, 0));

    EXPECT_EQ(CE_Failure,
              GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                  GDT_Unknown, 1, nullptr, 0, 0, 0));
    CPLErrorReset();
  }

  // 2. Valid RasterIO cases: noop window, multi-band read, forced cached IO,
  // pixel interleaved metadata, resampled reads with nodata / colortable /
  // overviews.
  EXPECT_EQ(CE_None, GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 0, 4, buf.data(),
                                         4, 4, GDT_Byte, 1, nullptr, 0, 0, 0));

  EXPECT_EQ(CE_None, GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf.data(),
                                         4, 4, GDT_Byte, 2, nullptr, 0, 0, 0));

  ds.bForceCachedIO = true;
  EXPECT_EQ(CE_None, GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf.data(),
                                         4, 4, GDT_Byte, 2, nullptr, 0, 0, 0));
  ds.bForceCachedIO = false;

  ds.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  EXPECT_EQ(CE_None, GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf.data(),
                                         4, 4, GDT_Byte, 2, nullptr, 0, 0, 0));
  ds.SetMetadataItem("INTERLEAVE", nullptr, "IMAGE_STRUCTURE");

  // Multi-band resampled read (GRIORA_Bilinear) with all bands OK.
  extra.eResampleAlg = GRIORA_Bilinear;
  EXPECT_EQ(CE_None,
            GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf.data(), 4, 4,
                                  GDT_Byte, 2, nullptr, 0, 0, 0, &extra));

  // Multi-band resampled read where band 1 is OK and band 2 has GMF_NODATA
  // (exercises partial nOKBands == 1 < nBandCount == 2).
  b2->custom_mask_flags_ = GMF_NODATA;
  EXPECT_EQ(CE_None,
            GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf.data(), 4, 4,
                                  GDT_Byte, 2, nullptr, 0, 0, 0, &extra));
  b2->custom_mask_flags_ = -1;

  // Band 2 has a ColorTable.
  b2->color_table_ = std::make_unique<GDALColorTable>();
  EXPECT_EQ(CE_None,
            GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf.data(), 4, 4,
                                  GDT_Byte, 2, nullptr, 0, 0, 0, &extra));
  b2->color_table_.reset();

  // Band 1 has overviews -> breaks out of loop at i==0 (nOKBands == 0).
  b1->ovr_count_ = 1;
  b1->overviews_.push_back(
      std::make_unique<CustomBand>(&ds, 1, GDT_Byte, 4, 1));
  b1->overviews_[0]->nRasterXSize = 4;
  b1->overviews_[0]->nRasterYSize = 4;
  EXPECT_EQ(CE_None,
            GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf.data(), 4, 4,
                                  GDT_Byte, 2, nullptr, 0, 0, 0, &extra));
  b1->ovr_count_ = 0;
  b1->overviews_.clear();

  // 3. AdviseRead tests.
  EXPECT_EQ(CE_None, GDALDatasetAdviseRead(hDS, 0, 0, 0, 4, 4, 4, GDT_Byte, 2,
                                           nullptr, nullptr));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALDatasetAdviseRead(hDS, -1, 0, 4, 4, 4, 4,
                                                GDT_Byte, 2, nullptr, nullptr));
    CPLErrorReset();
  }
  constexpr std::array<int, 2> kMap2 = {1, 2};
  EXPECT_EQ(CE_None,
            GDALDatasetAdviseRead(hDS, 0, 0, 4, 4, 4, 4, GDT_Byte, 2,
                                  const_cast<int*>(kMap2.data()), nullptr));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    b2->advise_err_ = CE_Failure;
    EXPECT_EQ(CE_Failure,
              GDALDatasetAdviseRead(hDS, 0, 0, 4, 4, 4, 4, GDT_Byte, 2,
                                    const_cast<int*>(kMap2.data()), nullptr));
    b2->advise_err_ = CE_None;
    CPLErrorReset();
  }

  // 4. Null band in middle of band array triggers error in
  // ValidateRasterIOOrAdviseReadParameters.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ExposedDataset sparse_ds;
    sparse_ds.RasterInitialize(4, 4);
    sparse_ds.SetBand(2, new CustomBand(&sparse_ds, 2));
    constexpr std::array<int, 1> kMapBand1 = {1};
    EXPECT_EQ(CE_Failure, sparse_ds.RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2,
                                             2, GDT_Byte, 1,
                                             const_cast<int*>(kMapBand1.data()),
                                             0, 0, 0, nullptr));
    CPLErrorReset();
  }
}

TEST(GdalDatasetTest, OpenOpenExOpenSharedDumpAndAsyncReader) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTempDir); };

  EXPECT_EQ(CE_None, GDALClose(nullptr));
  EXPECT_EQ(0, GDALDumpOpenDatasets(nullptr));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALOpenEx(nullptr, 0, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, GDALOpenShared(nullptr, GA_ReadOnly));

    // Incompatible GDAL_OF_THREAD_SAFE flags.
    constexpr char kNoTif[] = "/vsimem/gdaldataset_test/no.tif";
    EXPECT_EQ(nullptr,
              GDALOpenEx(kNoTif,
                         GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_UPDATE,
                         nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr,
              GDALOpenEx(kNoTif,
                         GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_VECTOR,
                         nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, GDALOpenEx(kNoTif,
                                  GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER |
                                      GDAL_OF_MULTIDIM_RASTER,
                                  nullptr, nullptr, nullptr));
    EXPECT_EQ(
        nullptr,
        GDALOpenEx(kNoTif, GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_GNM,
                   nullptr, nullptr, nullptr));

    // Incompatible GDAL_OF_SHARED | GDAL_OF_INTERNAL.
    EXPECT_EQ(nullptr, GDALOpenEx(kNoTif, GDAL_OF_SHARED | GDAL_OF_INTERNAL,
                                  nullptr, nullptr, nullptr));

    // Non-existent file with GDAL_OF_VERBOSE_ERROR and CACHE_KERCHUNK_JSON.
    constexpr std::array<const char*, 3> kKerchunkOpts = {
        "CACHE_KERCHUNK_JSON=YES", "@VALIDATE_OPEN_OPTIONS=NO", nullptr};
    EXPECT_EQ(nullptr,
              GDALOpenEx("/vsimem/gdaldataset_test/does_not_exist_12345.tif",
                         GDAL_OF_VERBOSE_ERROR, nullptr, kKerchunkOpts.data(),
                         nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Existing unrecognized file with GDAL_OF_VERBOSE_ERROR.
  {
    constexpr char kUnrecFile[] =
        "/vsimem/gdaldataset_test/unrecognized_file.bin";
    VsiMemTempWrapper unrec_wrapper(kUnrecFile, "NOT_A_VALID_FORMAT_HEADER");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALOpenEx(kUnrecFile, GDAL_OF_VERBOSE_ERROR, nullptr,
                                  nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Create a real GTiff with overviews and test GDALOpenShared, OVERVIEW_LEVEL,
  // GDAL_OF_THREAD_SAFE, GDALGetOpenDatasets, GDALDumpOpenDatasets,
  // AsyncReader.
  constexpr char kTifFile[] = "/vsimem/gdaldataset_test/open_test.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr ds_create(
      gtiff_drv->Create(kTifFile, 16, 16, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds_create);
  constexpr std::array<int, 1> kOvrFactors = {2};
  EXPECT_EQ(CE_None,
            ds_create->BuildOverviews("NEAREST", 1, kOvrFactors.data(), 0,
                                      nullptr, nullptr, nullptr, nullptr));
  ds_create.reset();

  // GDALOpenShared in update mode, then read-only mode (reuses update dataset).
  GDALDatasetUniquePtr shared1(
      GDALDataset::FromHandle(GDALOpenShared(kTifFile, GA_Update)));
  ASSERT_NE(nullptr, shared1);
  GDALDatasetUniquePtr shared2(
      GDALDataset::FromHandle(GDALOpenShared(kTifFile, GA_ReadOnly)));
  EXPECT_EQ(shared1.get(), shared2.get());

  // Explicitly test the GDALOpen() C API entry point to open a non-shared
  // dataset so GDALDumpOpenDatasets sees both shared and non-shared datasets.
  GDALDatasetUniquePtr non_shared(
      GDALDataset::FromHandle(GDALOpen(kTifFile, GA_ReadOnly)));
  ASSERT_NE(nullptr, non_shared);

  int open_count = 0;
  GDALDatasetH* open_list = nullptr;
  GDALGetOpenDatasets(&open_list, &open_count);
  EXPECT_GE(open_count, 2);

  char* dump_buf_raw = nullptr;
  size_t dump_size = 0;
  FILE* dump_fp = open_memstream(&dump_buf_raw, &dump_size);
  ASSERT_NE(nullptr, dump_fp);
  absl::Cleanup dump_fp_closer = [&dump_fp, &dump_buf_raw] {
    if (dump_fp != nullptr) {
      EXPECT_EQ(0, fclose(dump_fp));
    }
    std::free(dump_buf_raw);
  };
  EXPECT_GE(GDALDumpOpenDatasets(dump_fp), 2);
  EXPECT_EQ(0, fclose(dump_fp));
  dump_fp = nullptr;
  EXPECT_GT(dump_size, 0u);

  // AsyncReader on non_shared.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALBeginAsyncReader(nullptr, 0, 0, 4, 4, nullptr, 4, 4, GDT_Byte,
                                   1, nullptr, 0, 0, 0, nullptr));
    CPLErrorReset();
  }
  GDALEndAsyncReader(nullptr, nullptr);
  std::vector<GByte> async_buf(16, 0);
  GDALDatasetH h_non_shared = GDALDataset::ToHandle(non_shared.get());
  GDALAsyncReaderH h_async =
      GDALBeginAsyncReader(h_non_shared, 0, 0, 4, 4, async_buf.data(), 4, 4,
                           GDT_Byte, 1, nullptr, 0, 0, 0, nullptr);
  ASSERT_NE(nullptr, h_async);
  GDALEndAsyncReader(h_non_shared, h_async);

  EXPECT_EQ(CE_None, GDALClose(GDALDataset::ToHandle(non_shared.release())));
  // Decrements refcount from 2 to 1.
  EXPECT_EQ(CE_None, GDALClose(GDALDataset::ToHandle(shared2.release())));
  // Closes shared dataset.
  EXPECT_EQ(CE_None, GDALClose(GDALDataset::ToHandle(shared1.release())));

  // Open with OVERVIEW_LEVEL=0only, OVERVIEW_LEVEL=NONE, OVERVIEW_LEVEL=99.
  constexpr std::array<const char*, 2> kAllowedDrivers = {"GTiff", nullptr};
  constexpr std::array<const char*, 2> kOvr0Opts = {"OVERVIEW_LEVEL=0only",
                                                    nullptr};
  GDALDatasetUniquePtr ovr0_ds(GDALDataset::FromHandle(
      GDALOpenEx(kTifFile, GDAL_OF_RASTER | GDAL_OF_SHARED,
                 kAllowedDrivers.data(), kOvr0Opts.data(), nullptr)));
  ASSERT_NE(nullptr, ovr0_ds);
  EXPECT_EQ(8, GDALGetRasterXSize(GDALDataset::ToHandle(ovr0_ds.get())));
  ovr0_ds.reset();

  constexpr std::array<const char*, 2> kOvrNoneOpts = {"OVERVIEW_LEVEL=NONE",
                                                       nullptr};
  GDALDatasetUniquePtr ovr_none_ds(
      GDALDataset::Open(kTifFile, GDAL_OF_RASTER, kAllowedDrivers.data(),
                        kOvrNoneOpts.data(), nullptr));
  ASSERT_NE(nullptr, ovr_none_ds);
  EXPECT_NE(nullptr, ovr_none_ds->GetOpenOptions());
  EXPECT_NE(
      nullptr,
      const_cast<const GDALDataset*>(ovr_none_ds.get())->GetOpenOptions());
  ovr_none_ds.reset();

  constexpr std::array<const char*, 2> kOvrBadOpts = {"OVERVIEW_LEVEL=99",
                                                      nullptr};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALOpenEx(kTifFile, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR,
                         kAllowedDrivers.data(), kOvrBadOpts.data(), nullptr));
    CPLErrorReset();
  }

  // Open with GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_SHARED.
  GDALDatasetUniquePtr thread_safe_ds(GDALDataset::FromHandle(GDALOpenEx(
      kTifFile, GDAL_OF_RASTER | GDAL_OF_THREAD_SAFE | GDAL_OF_SHARED,
      kAllowedDrivers.data(), nullptr, nullptr)));
  ASSERT_NE(nullptr, thread_safe_ds);
  thread_safe_ds.reset();
}

TEST(GdalDatasetTest, ReportErrorAndDerivedSubdatasetsMetadata) {
  ExposedDataset ds;
  ds.RasterInitialize(4, 4);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ds.SetDescription("");
    ds.ReportError(CE_Warning, CPLE_AppDefined, "Empty desc warning %d", 1);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    ds.SetDescription("/vsimem/gdaldataset_test/my_ds.tif");
    ds.ReportError(CE_Warning, CPLE_AppDefined, "Named desc warning %d", 2);
    GDALDataset::ReportError("/vsimem/gdaldataset_test/static_ds.tif",
                             CE_Warning, CPLE_AppDefined, "Static warning %d",
                             3);
    GDALDataset::ReportUpdateNotSupportedByDriver("TESTDRV");
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 0 bands -> no DERIVED_SUBDATASETS domain.
  const StringListPtr domains0(ds.GetMetadataDomainList(), CSLDestroy);
  EXPECT_EQ(-1, CSLFindString(domains0.get(), "DERIVED_SUBDATASETS"));
  EXPECT_EQ(nullptr, ds.GetMetadata("DERIVED_SUBDATASETS"));

  // 1 real band.
  ds.SetBand(1, new CustomBand(&ds, 1, GDT_Byte));
  const StringListPtr domains1(ds.GetMetadataDomainList(), CSLDestroy);
  EXPECT_GE(CSLFindString(domains1.get(), "DERIVED_SUBDATASETS"), 0);
  char** derived_md1 = ds.GetMetadata("DERIVED_SUBDATASETS");
  EXPECT_NE(nullptr, derived_md1);
  const size_t count1 = CslToVector(derived_md1).size();

  // Add a complex band -> includes complex derived subdatasets too.
  ds.SetBand(2, new CustomBand(&ds, 2, GDT_CFloat32));
  char** derived_md2 = ds.GetMetadata("DERIVED_SUBDATASETS");
  EXPECT_GE(CslToVector(derived_md2).size(), count1);
}

TEST(GdalDatasetTest, VectorLayersCreateCopyAndIterators) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, GDALDatasetGetLayerCount(nullptr));
    EXPECT_EQ(nullptr, GDALDatasetGetLayer(nullptr, 0));
    EXPECT_EQ(nullptr, GDALDatasetGetLayerByName(nullptr, "a"));
    EXPECT_EQ(0, GDALDatasetIsLayerPrivate(nullptr, 0));
    EXPECT_EQ(OGRERR_INVALID_HANDLE, GDALDatasetDeleteLayer(nullptr, 0));
    EXPECT_EQ(nullptr,
              GDALDatasetCreateLayer(nullptr, "a", nullptr, wkbPoint, nullptr));
    EXPECT_EQ(nullptr, GDALDatasetCreateLayerFromGeomFieldDefn(
                           nullptr, "a", nullptr, nullptr));
    EXPECT_EQ(nullptr, GDALDatasetCopyLayer(nullptr, nullptr, "a", nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // Base class defaults, null layer name checks, and CopyLayer without
  // ODsCCreateLayer.
  struct MinimalDataset : public GDALDataset {
  } base_ds;
  EXPECT_EQ(0, base_ds.GetLayerCount());
  EXPECT_EQ(nullptr, base_ds.GetLayer(0));
  EXPECT_FALSE(base_ds.IsLayerPrivate(0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, base_ds.DeleteLayer(0));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, base_ds.CreateLayer("l1"));
    EXPECT_EQ(nullptr, base_ds.CreateLayer("l1", nullptr));

    EXPECT_EQ(nullptr,
              GDALDatasetCreateLayer(hDS, nullptr, nullptr, wkbPoint, nullptr));
    EXPECT_EQ(nullptr, GDALDatasetCreateLayerFromGeomFieldDefn(
                           hDS, nullptr, nullptr, nullptr));

    CustomLayer src_lyr("src", wkbCurvePolygon);
    EXPECT_EQ(nullptr, ds.CopyLayer(&src_lyr, "dst", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Enable layer creation on ds.
  ds.allow_create_layer_ = true;
  ds.allow_delete_layer_ = true;
  ds.SetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST,
                     "<LayerCreationOptionList>"
                     "  <Option name='OPT1' type='string'/>"
                     "</LayerCreationOptionList>");

  constexpr std::array<const char*, 2> kLco = {"OPT1=VAL", nullptr};
  EXPECT_NE(nullptr, GDALDatasetCreateLayer(hDS, "LayerOne", nullptr, wkbNone,
                                            const_cast<char**>(kLco.data())));
  // CreateLayer with non-linear geometry when ODsCCurveGeometries is false
  // linearizes the geometry type.
  EXPECT_NE(nullptr, GDALDatasetCreateLayer(hDS, "LayerTwo", nullptr,
                                            wkbCircularString, nullptr));
  EXPECT_EQ(wkbLineString, ds.GetLayer(1)->GetGeomType());

  OGRGeomFieldDefn gfd("geom", wkbPoint);
  EXPECT_NE(nullptr,
            GDALDatasetCreateLayerFromGeomFieldDefn(
                hDS, "LayerThree", OGRGeomFieldDefn::ToHandle(&gfd), nullptr));

  EXPECT_EQ(3, GDALDatasetGetLayerCount(hDS));
  EXPECT_NE(nullptr, GDALDatasetGetLayer(hDS, 0));
  EXPECT_EQ(0, GDALDatasetIsLayerPrivate(hDS, 0));

  // GetLayerByName & GetLayerIndex: exact match, case-insensitive match, not
  // found, null.
  EXPECT_EQ(nullptr, ds.GetLayerByName(nullptr));
  EXPECT_NE(nullptr, GDALDatasetGetLayerByName(hDS, "LayerOne"));
  EXPECT_NE(nullptr, GDALDatasetGetLayerByName(hDS, "layerone"));
  EXPECT_EQ(nullptr, GDALDatasetGetLayerByName(hDS, "NoSuchLayer"));
  EXPECT_EQ(1, ds.GetLayerIndex("LayerTwo"));
  EXPECT_EQ(1, ds.GetLayerIndex("layertwo"));
  EXPECT_EQ(-1, ds.GetLayerIndex("NoSuchLayer"));

  // Layers and ConstLayers iterators & copy/move/post-increment/indexing.
  auto layers = ds.GetLayers();
  EXPECT_EQ(3U, layers.size());
  EXPECT_NE(nullptr, layers[0]);
  EXPECT_NE(nullptr, layers[static_cast<size_t>(1)]);
  EXPECT_NE(nullptr, layers["LayerThree"]);
  GDALDataset::Layers::Iterator it_default;
  auto it1 = layers.begin();
  auto it_copy(it1);
  it_default = it_copy;
  auto it_move(std::move(it_copy));
  it_default = std::move(it_move);
  auto it_post = it1++;
  EXPECT_NE(*it_post, *it1);
  int lyr_cnt = 0;
  for (OGRLayer* l : layers) {
    EXPECT_NE(nullptr, l);
    ++lyr_cnt;
  }
  EXPECT_EQ(3, lyr_cnt);

  const ExposedDataset& const_ds = ds;
  auto const_layers = const_ds.GetLayers();
  EXPECT_EQ(3U, const_layers.size());
  EXPECT_NE(nullptr, const_layers[0]);
  EXPECT_NE(nullptr, const_layers[static_cast<size_t>(1)]);
  EXPECT_NE(nullptr, const_layers["LayerThree"]);
  GDALDataset::ConstLayers::Iterator cit_default;
  auto cit1 = const_layers.begin();
  auto cit_copy(cit1);
  cit_default = cit_copy;
  auto cit_move(std::move(cit_copy));
  cit_default = std::move(cit_move);
  auto cit_post = cit1++;
  EXPECT_NE(*cit_post, *cit1);
  lyr_cnt = 0;
  for (const OGRLayer* l : const_layers) {
    EXPECT_NE(nullptr, l);
    ++lyr_cnt;
  }
  EXPECT_EQ(3, lyr_cnt);

  // CopyLayer tests:
  // 1. Layer with 1 geom field, fields, metadata, and DST_SRSWKT reprojection.
  OGRSpatialReference srs_src;
  srs_src.SetWellKnownGeogCS("WGS84");
  srs_src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  CustomLayer copy_src("copy_src", wkbPoint, &srs_src);
  copy_src.SetMetadataItem("META_K", "META_V");
  OGRFieldDefn f_id("id", OFTInteger);
  OGRFieldDefn f_name("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, copy_src.CreateField(&f_id, TRUE));
  EXPECT_EQ(OGRERR_NONE, copy_src.CreateField(&f_name, TRUE));
  auto feat = std::make_unique<OGRFeature>(copy_src.GetLayerDefn());
  feat->SetFID(10);
  feat->SetField("id", 42);
  feat->SetField("name", "pt1");
  OGRPoint pt(2.0, 49.0);
  feat->SetGeometry(&pt);
  EXPECT_EQ(OGRERR_NONE, copy_src.CreateFeature(feat.get()));

  // Pre-create 'id' field on destination layer and test
  // null_defn_before_field_.
  ds.precreated_fields_.push_back(f_id);
  ds.created_layers_null_defn_before_field_ = true;
  OGRSpatialReference srs_dst;
  srs_dst.importFromEPSG(3857);
  char* dst_wkt_raw = nullptr;
  ASSERT_EQ(OGRERR_NONE, srs_dst.exportToWkt(&dst_wkt_raw));
  const std::unique_ptr<char, CplFreeDeleter> dst_wkt(dst_wkt_raw);
  CPLStringList copy_opts;
  copy_opts.SetNameValue("DST_SRSWKT", dst_wkt.get());
  copy_opts.SetNameValue("COPY_MD", "YES");

  OGRLayerH h_copied = GDALDatasetCopyLayer(hDS, OGRLayer::ToHandle(&copy_src),
                                            "CopiedOne", copy_opts.List());
  ASSERT_NE(nullptr, h_copied);
  EXPECT_STREQ("META_V",
               OGRLayer::FromHandle(h_copied)->GetMetadataItem("META_K"));
  ds.precreated_fields_.clear();
  ds.created_layers_null_defn_before_field_ = false;

  // 2. CopyLayer with transactions enabled on destination layer and multiple
  // geom fields.
  CustomLayer copy_src2("copy_src2", wkbNone);
  OGRGeomFieldDefn gfd1("geom1", wkbPoint);
  OGRGeomFieldDefn gfd2("geom2", wkbPoint);
  EXPECT_EQ(OGRERR_NONE, copy_src2.CreateGeomField(&gfd1, TRUE));
  EXPECT_EQ(OGRERR_NONE, copy_src2.CreateGeomField(&gfd2, TRUE));
  auto feat2 = std::make_unique<OGRFeature>(copy_src2.GetLayerDefn());
  feat2->SetFID(10);
  OGRPoint pt2(2.0, 49.0);
  feat2->SetGeomField(0, &pt2);
  feat2->SetGeomField(1, &pt2);
  EXPECT_EQ(OGRERR_NONE, copy_src2.CreateFeature(feat2.get()));

  ds.allow_create_geom_field_ = true;
  ds.created_layers_support_tx_ = true;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    ds.created_layers_skip_adding_field_ = true;  // Triggers driver warning.
    EXPECT_NE(nullptr, ds.CopyLayer(&copy_src2, "CopiedTx", nullptr));

    // 3. CopyLayer with transactions where CreateFeature fails ->
    // RollbackTransaction.
    ds.created_layers_create_feature_err_ = OGRERR_FAILURE;
    OGRLayer* copied_rollback =
        ds.CopyLayer(&copy_src2, "CopiedRollback", nullptr);
    ASSERT_NE(nullptr, copied_rollback);
    EXPECT_TRUE(static_cast<CustomLayer*>(copied_rollback)->rollback_called_);
    ds.created_layers_create_feature_err_ = OGRERR_NONE;
    ds.created_layers_support_tx_ = false;
    ds.created_layers_skip_adding_field_ = false;
    CPLErrorReset();
  }

  // DeleteLayer.
  EXPECT_EQ(OGRERR_NONE, GDALDatasetDeleteLayer(hDS, 0));
}

TEST(GdalDatasetTest, ExecuteSQLDDLAndSelectComprehensive) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetReleaseResultSet(nullptr, nullptr);
    EXPECT_EQ(nullptr,
              GDALDatasetExecuteSQL(nullptr, "SELECT 1", nullptr, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, GDALDatasetAbortSQL(nullptr));
    CPLErrorReset();
  }

  EXPECT_EQ(TRUE, GDALDataset::IsGenericSQLDialect("OGRSQL"));
  EXPECT_EQ(TRUE, GDALDataset::IsGenericSQLDialect("SQLITE"));
  EXPECT_EQ(FALSE, GDALDataset::IsGenericSQLDialect("NATIVE"));
  EXPECT_EQ(FALSE, GDALDataset::IsGenericSQLDialect(nullptr));

  ExposedDataset ds;
  ds.allow_delete_layer_ = true;
  ds.SetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS, "OGRSQL SQLITE CUSTOM");
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, GDALDatasetAbortSQL(hDS));

  auto lyr = std::make_unique<CustomLayer>("tbl", wkbPoint);
  CustomLayer* lyr_ptr = lyr.get();
  ds.layers_.push_back(std::move(lyr));

  struct SqlCase {
    const char* sql;
    CPLErr expected;
  };
  auto run_sql_cases = [hDS](std::span<const SqlCase> cases,
                             const char* dialect = nullptr) {
    WithQuietHandler quiet;
    for (const auto& tc : cases) {
      SCOPED_TRACE(tc.sql);
      CPLErrorReset();
      OGRLayerH res = GDALDatasetExecuteSQL(hDS, tc.sql, nullptr, dialect);
      // An unrecognized dialect emits a CE_Warning before falling back to
      // OGRSQL; subsequent errors overwrite CPLGetLastErrorType with
      // CE_Failure.
      const CPLErr expected_err = (tc.expected == CE_None && dialect != nullptr)
                                      ? CE_Warning
                                      : tc.expected;
      EXPECT_EQ(expected_err, CPLGetLastErrorType());
      if (res != nullptr) {
        GDALDatasetReleaseResultSet(hDS, res);
      }
    }
    CPLErrorReset();
  };

  // 1. ALTER TABLE ADD [COLUMN] covering all SQL types in
  // GDALDatasetParseSQLType.
  constexpr std::array<SqlCase, 13> kAddColCases = {{
      {.sql = "ALTER TABLE tbl ADD col_int INTEGER", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD COLUMN col_int_arr INTEGER[]",
       .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_flt FLOAT", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_num NUMERIC(10, 2)",
       .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_flt_arr FLOAT[]", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_str CHARACTER(20)", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_str_arr TEXT[]", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_date DATE", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_time TIME", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_dt TIMESTAMP", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD col_unk UNKNOWN_TYPE", .expected = CE_None},
      {.sql = "ALTER TABLE tbl ADD bad_syntax", .expected = CE_Failure},
      {.sql = "ALTER TABLE no_such_tbl ADD col_x INTEGER",
       .expected = CE_Failure},
  }};
  run_sql_cases(kAddColCases, "UNSUPPORTED_DIALECT");
  EXPECT_GE(lyr_ptr->GetLayerDefn()->GetFieldCount(), 10);

  // Add Int64 and Boolean subtypes so BuildParseInfo covers them.
  OGRFieldDefn f_bool("col_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, lyr_ptr->CreateField(&f_bool, TRUE));
  OGRFieldDefn f_i64("col_i64", OFTInteger64);
  EXPECT_EQ(OGRERR_NONE, lyr_ptr->CreateField(&f_i64, TRUE));
  OGRFieldDefn f_i64_bool("col_i64_bool", OFTInteger64);
  f_i64_bool.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, lyr_ptr->CreateField(&f_i64_bool, TRUE));
  lyr_ptr->SetMetadataItem(OLMD_FID64, "YES");
  lyr_ptr->fid_col_ = "ogc_fid";

  // 2. ALTER TABLE RENAME / ALTER / DROP / RENAME TO, CREATE INDEX, DROP INDEX.
  constexpr std::array<SqlCase, 25> kDdlCases = {{
      // ALTER TABLE RENAME [COLUMN].
      {.sql = "ALTER TABLE tbl RENAME COLUMN col_unk TO col_ren1",
       .expected = CE_None},
      {.sql = "ALTER TABLE tbl RENAME col_ren1 TO col_ren2",
       .expected = CE_None},
      {.sql = "ALTER TABLE tbl RENAME bad_syntax", .expected = CE_Failure},
      {.sql = "ALTER TABLE no_tbl RENAME a TO b", .expected = CE_Failure},
      {.sql = "ALTER TABLE tbl RENAME no_col TO b", .expected = CE_Failure},

      // ALTER TABLE ALTER [COLUMN].
      {.sql = "ALTER TABLE tbl ALTER COLUMN col_ren2 TYPE VARCHAR(30)",
       .expected = CE_None},
      // Same type and width -> l_nFlags == 0.
      {.sql = "ALTER TABLE tbl ALTER col_ren2 TYPE VARCHAR(30)",
       .expected = CE_None},
      {.sql = "ALTER TABLE tbl ALTER col_ren2 TYPE INTEGER",
       .expected = CE_None},
      {.sql = "ALTER TABLE tbl ALTER bad_syntax", .expected = CE_Failure},
      {.sql = "ALTER TABLE no_tbl ALTER a TYPE INTEGER",
       .expected = CE_Failure},
      {.sql = "ALTER TABLE tbl ALTER no_col TYPE INTEGER",
       .expected = CE_Failure},

      // ALTER TABLE DROP [COLUMN].
      {.sql = "ALTER TABLE tbl DROP COLUMN col_ren2", .expected = CE_None},
      {.sql = "ALTER TABLE tbl DROP col_dt", .expected = CE_None},
      {.sql = "ALTER TABLE tbl DROP", .expected = CE_Failure},
      {.sql = "ALTER TABLE no_tbl DROP a", .expected = CE_Failure},
      {.sql = "ALTER TABLE tbl DROP no_col", .expected = CE_Failure},

      // ALTER TABLE RENAME TO and unsupported ALTER TABLE.
      {.sql = "ALTER TABLE tbl RENAME TO tbl_renamed", .expected = CE_None},
      {.sql = "ALTER TABLE tbl_renamed RENAME TO tbl", .expected = CE_None},
      {.sql = "ALTER TABLE no_tbl RENAME TO tbl2", .expected = CE_Failure},
      {.sql = "ALTER TABLE tbl FOOBAR BAZ", .expected = CE_Failure},

      // CREATE INDEX and DROP INDEX when layer lacks index support.
      {.sql = "CREATE INDEX bad syntax", .expected = CE_Failure},
      {.sql = "CREATE INDEX ON no_tbl USING col_int", .expected = CE_Failure},
      {.sql = "CREATE INDEX ON tbl USING col_int", .expected = CE_Failure},
      {.sql = "DROP INDEX bad syntax", .expected = CE_Failure},
      {.sql = "DROP INDEX ON no_tbl", .expected = CE_Failure},
  }};
  run_sql_cases(kDdlCases);
  constexpr std::array<SqlCase, 1> kDropIdxUnsupportedCases = {{
      {.sql = "DROP INDEX ON tbl", .expected = CE_Failure},
  }};
  run_sql_cases(kDropIdxUnsupportedCases);

  // Enable index support on tbl (DummyIndex returns OGRERR_FAILURE without
  // emitting a CPLError, exercising the silent error return branch).
  lyr_ptr->EnableDummyIndex();
  constexpr std::array<SqlCase, 2> kIdxCreateCases = {{
      {.sql = "CREATE INDEX ON tbl USING no_col", .expected = CE_None},
      {.sql = "CREATE INDEX ON tbl USING col_int", .expected = CE_None},
  }};
  run_sql_cases(kIdxCreateCases);

  lyr_ptr->dummy_idx_->create_err_ = OGRERR_FAILURE;
  constexpr std::array<SqlCase, 1> kIdxCreateFailCases = {{
      {.sql = "CREATE INDEX ON tbl USING col_int", .expected = CE_Failure},
  }};
  run_sql_cases(kIdxCreateFailCases);
  lyr_ptr->dummy_idx_->create_err_ = OGRERR_NONE;

  constexpr std::array<SqlCase, 3> kIdxDropCases = {{
      {.sql = "DROP INDEX ON tbl USING no_col", .expected = CE_None},
      {.sql = "DROP INDEX ON tbl USING col_int", .expected = CE_None},
      {.sql = "DROP INDEX ON tbl", .expected = CE_None},
  }};
  run_sql_cases(kIdxDropCases);

  lyr_ptr->dummy_idx_->drop_err_ = OGRERR_FAILURE;
  constexpr std::array<SqlCase, 1> kIdxDropFailCases = {{
      {.sql = "DROP INDEX ON tbl", .expected = CE_None},
  }};
  run_sql_cases(kIdxDropFailCases);
  lyr_ptr->dummy_idx_->drop_err_ = OGRERR_NONE;

  // 3. SELECT, UNION ALL, JOIN, and SQLite dialect.
  auto feat = std::make_unique<OGRFeature>(lyr_ptr->GetLayerDefn());
  feat->SetFID(1);
  feat->SetField("col_int", 10);
  EXPECT_EQ(OGRERR_NONE, lyr_ptr->CreateFeature(feat.get()));

  // Preparse error and non-existent table.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALDatasetExecuteSQL(hDS, "SELECT FROM", nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(nullptr, GDALDatasetExecuteSQL(hDS, "SELECT * FROM no_such_tbl",
                                             nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Valid SELECT with WHERE.
  OGRLayerH h_res = GDALDatasetExecuteSQL(
      hDS, "SELECT *, ogc_fid FROM tbl WHERE col_int = 10", nullptr, nullptr);
  ASSERT_NE(nullptr, h_res);
  GDALDatasetReleaseResultSet(hDS, h_res);

  // UNION ALL (valid and failing second leg).
  OGRLayerH h_union = GDALDatasetExecuteSQL(
      hDS, "SELECT col_int FROM tbl UNION ALL SELECT col_int FROM tbl", nullptr,
      nullptr);
  ASSERT_NE(nullptr, h_union);
  GDALDatasetReleaseResultSet(hDS, h_union);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALDatasetExecuteSQL(
                  hDS, "SELECT col_int FROM tbl UNION ALL SELECT * FROM no_tbl",
                  nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    // Secondary datasource JOIN failure.
    EXPECT_EQ(nullptr,
              GDALDatasetExecuteSQL(hDS,
                                    "SELECT * FROM tbl LEFT JOIN "
                                    "'/vsimem/gdaldataset_test/no_ds.json'.l2 "
                                    "ON tbl.col_int = l2.id",
                                    nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // SQLite dialect (SQLite driver is enabled in //third_party/gdal).
  OGRLayerH h_sqlite =
      GDALDatasetExecuteSQL(hDS, "SELECT col_int FROM tbl", nullptr, "SQLite");
  ASSERT_NE(nullptr, h_sqlite);
  GDALDatasetReleaseResultSet(hDS, h_sqlite);

  // 4. DROP TABLE.
  constexpr std::array<SqlCase, 3> kDropTableCases = {{
      {.sql = "DROP TABLE bad syntax extra", .expected = CE_Failure},
      {.sql = "DROP TABLE no_such_tbl", .expected = CE_Failure},
      {.sql = "DROP TABLE tbl", .expected = CE_None},
  }};
  run_sql_cases(kDropTableCases);
  EXPECT_EQ(0, ds.GetLayerCount());
}

TEST(GdalDatasetTest, StyleTableGetNextFeatureGetFeaturesAndTransactions) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDatasetGetStyleTable(nullptr));
    GDALDatasetSetStyleTableDirectly(nullptr, nullptr);
    GDALDatasetSetStyleTable(nullptr, nullptr);
    GDALDatasetResetReading(nullptr);
    EXPECT_EQ(nullptr, GDALDatasetGetNextFeature(nullptr, nullptr, nullptr,
                                                 nullptr, nullptr));
    EXPECT_EQ(0, GDALDatasetTestCapability(nullptr, ODsCCreateLayer));
    EXPECT_EQ(OGRERR_INVALID_HANDLE,
              GDALDatasetStartTransaction(nullptr, FALSE));
    EXPECT_EQ(OGRERR_INVALID_HANDLE, GDALDatasetCommitTransaction(nullptr));
    EXPECT_EQ(OGRERR_INVALID_HANDLE, GDALDatasetRollbackTransaction(nullptr));
    EXPECT_FALSE(GDALDatasetSetQueryLoggerFunc(nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // 1. StyleTable.
  EXPECT_EQ(nullptr, GDALDatasetGetStyleTable(hDS));
  OGRStyleTable st;
  st.AddStyle("s1", "PEN(c:#FF0000)");
  GDALDatasetSetStyleTable(hDS, reinterpret_cast<OGRStyleTableH>(&st));
  EXPECT_NE(nullptr, GDALDatasetGetStyleTable(hDS));
  // Replace style table.
  GDALDatasetSetStyleTable(hDS, reinterpret_cast<OGRStyleTableH>(&st));
  auto st_direct = std::make_unique<OGRStyleTable>();
  OGRStyleTable* st_direct_ptr = st_direct.get();
  GDALDatasetSetStyleTableDirectly(
      hDS, reinterpret_cast<OGRStyleTableH>(st_direct.release()));
  EXPECT_EQ(st_direct_ptr, ds.GetStyleTable());

  // 2. Transactions and QueryLogger default implementations.
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            GDALDatasetStartTransaction(hDS, FALSE));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, GDALDatasetCommitTransaction(hDS));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, GDALDatasetRollbackTransaction(hDS));
  EXPECT_FALSE(GDALDatasetSetQueryLoggerFunc(hDS, nullptr, nullptr));
  EXPECT_EQ(0, GDALDatasetTestCapability(hDS, nullptr));

  // 3. GetNextFeature and GetFeatures() across 2 layers.
  auto l1 = std::make_unique<CustomLayer>("l1");
  auto f1 = std::make_unique<OGRFeature>(l1->GetLayerDefn());
  f1->SetFID(1);
  EXPECT_EQ(OGRERR_NONE, l1->CreateFeature(f1.get()));

  auto l2 = std::make_unique<CustomLayer>("l2");
  auto f2 = std::make_unique<OGRFeature>(l2->GetLayerDefn());
  f2->SetFID(2);
  EXPECT_EQ(OGRERR_NONE, l2->CreateFeature(f2.get()));

  CustomLayer* l1_ptr = l1.get();
  ds.layers_.push_back(std::move(l1));
  ds.layers_.push_back(std::move(l2));

  // Case 1: OLCFastFeatureCount is true on all layers.
  GDALDatasetResetReading(hDS);
  OGRLayerH h_belong = nullptr;
  double pct = 0.0;
  OGRFeatureUniquePtr feat1(OGRFeature::FromHandle(GDALDatasetGetNextFeature(
      hDS, &h_belong, &pct, GDALDummyProgress, nullptr)));
  ASSERT_NE(nullptr, feat1);
  EXPECT_DOUBLE_EQ(0.5, pct);

  OGRFeatureUniquePtr feat2(OGRFeature::FromHandle(GDALDatasetGetNextFeature(
      hDS, &h_belong, &pct, GDALDummyProgress, nullptr)));
  ASSERT_NE(nullptr, feat2);
  EXPECT_DOUBLE_EQ(1.0, pct);

  // EOF reached.
  EXPECT_EQ(nullptr, GDALDatasetGetNextFeature(hDS, &h_belong, &pct,
                                               GDALDummyProgress, nullptr));
  // Subsequent call after EOF (nCurrentLayerIdx < 0).
  EXPECT_EQ(nullptr, GDALDatasetGetNextFeature(hDS, &h_belong, &pct,
                                               GDALDummyProgress, nullptr));

  // Case 2: OLCFastFeatureCount is false on l1 -> nTotalFeatures < 0.
  l1_ptr->fast_feature_count_ = false;
  GDALDatasetResetReading(hDS);
  for (int i = 0; i < 2; ++i) {
    OGRFeatureUniquePtr hf(OGRFeature::FromHandle(GDALDatasetGetNextFeature(
        hDS, &h_belong, &pct, GDALDummyProgress, nullptr)));
    ASSERT_NE(nullptr, hf);
  }

  // Case 3: GetFeatures() range-for loop.
  int feat_count = 0;
  for (auto&& pair : ds.GetFeatures()) {
    EXPECT_NE(nullptr, pair.feature);
    EXPECT_NE(nullptr, pair.layer);
    ++feat_count;
  }
  EXPECT_EQ(2, feat_count);
}

TEST(GdalDatasetTest, ReadWriteMutexAndParentLockSharing) {
  ExposedDataset parent;
  parent.eAccess = GA_Update;
  parent.RasterInitialize(4, 4);
  parent.SetBand(1, new CustomBand(&parent, 1));

  parent.InitRWLock();
  EXPECT_EQ(TRUE, parent.EnterReadWrite(GF_Read));
  parent.TemporarilyDropReadWriteLock();
  parent.ReacquireReadWriteLock();
  parent.LeaveReadWrite();

  EXPECT_TRUE(parent.AcquireMutex());
  parent.ReleaseMutex();

  // Child sharing lock with parent.
  ExposedDataset child;
  child.ShareLockWithParentDataset(&parent);
  child.InitRWLock();
  EXPECT_EQ(TRUE, child.EnterReadWrite(GF_Write));
  child.TemporarilyDropReadWriteLock();
  child.ReacquireReadWriteLock();
  child.LeaveReadWrite();
  EXPECT_TRUE(child.AcquireMutex());
  child.ReleaseMutex();
  child.DisableReadWriteMutex();

  // Standalone dataset with GDAL_ENABLE_READ_WRITE_MUTEX=NO.
  {
    CPLConfigOptionSetter no_mutex("GDAL_ENABLE_READ_WRITE_MUTEX", "NO", false);
    ExposedDataset ds_nomutex;
    ds_nomutex.eAccess = GA_Update;
    EXPECT_EQ(FALSE, ds_nomutex.EnterReadWrite(GF_Write));
  }
}

TEST(GdalDatasetTest, FieldDomainsRelationshipsCompressionAndClearStats) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetClearStatistics(nullptr);
    EXPECT_EQ(nullptr, GDALDatasetGetFieldDomainNames(nullptr, nullptr));
    EXPECT_EQ(nullptr, GDALDatasetGetFieldDomain(nullptr, "d"));
    EXPECT_FALSE(GDALDatasetAddFieldDomain(nullptr, nullptr, nullptr));
    EXPECT_FALSE(GDALDatasetDeleteFieldDomain(nullptr, "d", nullptr));
    EXPECT_FALSE(GDALDatasetUpdateFieldDomain(nullptr, nullptr, nullptr));

    EXPECT_EQ(nullptr, GDALDatasetGetRelationshipNames(nullptr, nullptr));
    EXPECT_EQ(nullptr, GDALDatasetGetRelationship(nullptr, "r"));
    EXPECT_FALSE(GDALDatasetAddRelationship(nullptr, nullptr, nullptr));
    EXPECT_FALSE(GDALDatasetDeleteRelationship(nullptr, "r", nullptr));
    EXPECT_FALSE(GDALDatasetUpdateRelationship(nullptr, nullptr, nullptr));

    EXPECT_EQ(nullptr, GDALDatasetGetCompressionFormats(nullptr, 0, 0, 1, 1, 1,
                                                        nullptr));
    EXPECT_EQ(CE_Failure, GDALDatasetReadCompressedData(
                              nullptr, "JPEG", 0, 0, 1, 1, 1, nullptr, nullptr,
                              nullptr, nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  ds.RasterInitialize(4, 4);
  ds.SetBand(1, new CustomBand(&ds, 1));
  ds.SetBand(2, new CustomBand(&ds, 2));
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // 1. ClearStatistics without and with RootGroup.
  EXPECT_EQ(nullptr, ds.GetRootGroup());
  GDALDatasetClearStatistics(hDS);
  ds.root_group_ = std::make_shared<DummyGroup>();
  GDALDatasetClearStatistics(hDS);
  EXPECT_TRUE(ds.root_group_->cleared_);

  // 2. RawBinaryLayout.
  GDALDataset::RawBinaryLayout layout;
  EXPECT_FALSE(ds.GetRawBinaryLayout(layout));

  // 3. SetEnableOverviews / AreOverviewsEnabled / IsAllBands.
  EXPECT_TRUE(ds.AreOverviewsEnabled());
  ds.SetEnableOverviews(false);
  EXPECT_FALSE(ds.AreOverviewsEnabled());
  ds.SetEnableOverviews(true);

  constexpr std::array<int, 2> kBandsOk = {1, 2};
  constexpr std::array<int, 2> kBandsBad = {2, 1};
  EXPECT_TRUE(ds.IsAllBands(2, nullptr));
  EXPECT_TRUE(ds.IsAllBands(2, kBandsOk.data()));
  EXPECT_FALSE(ds.IsAllBands(1, nullptr));
  EXPECT_FALSE(ds.IsAllBands(2, kBandsBad.data()));

  // 4. FieldDomains: populate m_oMapFieldDomains to test GetFieldDomainNames
  // and GetFieldDomain.
  OGRField min_val = {};
  min_val.Integer = 0;
  OGRField max_val = {};
  max_val.Integer = 10;
  auto range_dom = std::make_unique<OGRRangeFieldDomain>(
      "dom1", "desc", OFTInteger, OFSTNone, min_val, true, max_val, true);
  OGRFieldDomainH h_dom = OGRFieldDomain::ToHandle(range_dom.get());

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    char* raw_reason = nullptr;
    EXPECT_FALSE(GDALDatasetAddFieldDomain(hDS, h_dom, &raw_reason));
    std::unique_ptr<char, CplFreeDeleter> reason(raw_reason);
    EXPECT_NE(nullptr, reason);

    raw_reason = nullptr;
    EXPECT_FALSE(GDALDatasetUpdateFieldDomain(hDS, h_dom, &raw_reason));
    reason.reset(raw_reason);
    EXPECT_NE(nullptr, reason);

    raw_reason = nullptr;
    EXPECT_FALSE(GDALDatasetDeleteFieldDomain(hDS, "dom1", &raw_reason));
    reason.reset(raw_reason);
    EXPECT_NE(nullptr, reason);
    CPLErrorReset();
  }

  ds.m_oMapFieldDomains["dom1"] = std::move(range_dom);
  const StringListPtr dom_names(GDALDatasetGetFieldDomainNames(hDS, nullptr),
                                CSLDestroy);
  EXPECT_EQ(1U, CslToVector(dom_names.get()).size());
  EXPECT_NE(nullptr, GDALDatasetGetFieldDomain(hDS, "dom1"));
  EXPECT_EQ(nullptr, GDALDatasetGetFieldDomain(hDS, "no_such_dom"));

  // 5. Relationships.
  EXPECT_EQ(nullptr, GDALDatasetGetRelationshipNames(hDS, nullptr));
  EXPECT_EQ(nullptr, GDALDatasetGetRelationship(hDS, "rel1"));
  GDALRelationship rel("rel1", "left_t", "right_t",
                       GDALRelationshipCardinality::GRC_ONE_TO_MANY);
  GDALRelationshipH h_rel = GDALRelationship::ToHandle(&rel);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    char* raw_reason = nullptr;
    EXPECT_FALSE(GDALDatasetAddRelationship(hDS, h_rel, &raw_reason));
    std::unique_ptr<char, CplFreeDeleter> reason(raw_reason);
    EXPECT_NE(nullptr, reason);

    raw_reason = nullptr;
    EXPECT_FALSE(GDALDatasetUpdateRelationship(hDS, h_rel, &raw_reason));
    reason.reset(raw_reason);
    EXPECT_NE(nullptr, reason);

    raw_reason = nullptr;
    EXPECT_FALSE(GDALDatasetDeleteRelationship(hDS, "rel1", &raw_reason));
    reason.reset(raw_reason);
    EXPECT_NE(nullptr, reason);
    CPLErrorReset();
  }

  // 6. CompressionFormats and ReadCompressedData.
  EXPECT_EQ(nullptr,
            GDALDatasetGetCompressionFormats(hDS, 0, 0, 4, 4, 2, nullptr));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDatasetReadCompressedData(hDS, "JPEG", 0, 0, 4, 4, 2, nullptr,
                                            nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  // 7. CanBeCloned and Clone.
  EXPECT_FALSE(ds.CanBeCloned(GDAL_OF_RASTER, true));
  ds.m_bCanBeReopened = true;
  EXPECT_TRUE(ds.CanBeCloned(GDAL_OF_RASTER, true));
  EXPECT_FALSE(ds.CanBeCloned(GDAL_OF_VECTOR, true));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.Clone(GDAL_OF_RASTER, true));
    CPLErrorReset();
  }
}

TEST(GdalDatasetTest, GeolocationToPixelLineGetExtentAndBuildFilename) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDatasetGeolocationToPixelLine(nullptr, 0, 0, nullptr, nullptr,
                                                nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALGetExtent(nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALGetExtentWGS84LongLat(nullptr, nullptr));
    CPLErrorReset();
  }

  ExposedDataset ds;
  ds.RasterInitialize(10, 10);
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // Without georeferencing, GeolocationToPixelLine and GetExtent fail.
  double px = 0.0;
  double line = 0.0;
  OGREnvelope env;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALDatasetGeolocationToPixelLine(
                              hDS, 0, 0, nullptr, &px, &line, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CE_Failure, GDALGetExtent(hDS, &env, nullptr));
    CPLErrorReset();
  }

  // Set WGS84 SRS and GeoTransform on ds.
  ds.use_custom_srs_ = true;
  ds.custom_srs_ = std::make_unique<OGRSpatialReference>();
  ds.custom_srs_->SetWellKnownGeogCS("WGS84");
  ds.custom_srs_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds.has_custom_gt_ = true;
  ds.custom_gt_ = GDALGeoTransform(10.0, 1.0, 0.0, 50.0, 0.0, -1.0);

  // GeolocationToPixelLine with null SRS, TRADITIONAL_GIS_ORDER,
  // AUTHORITY_COMPLIANT, and OAMS_CUSTOM.
  EXPECT_EQ(CE_None, GDALDatasetGeolocationToPixelLine(hDS, 15.0, 45.0, nullptr,
                                                       &px, &line, nullptr));
  EXPECT_NEAR(5.0, px, 1e-5);
  EXPECT_NEAR(5.0, line, 1e-5);

  OGRSpatialReference srs_trad;
  srs_trad.SetWellKnownGeogCS("WGS84");
  srs_trad.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(CE_None,
            ds.GeolocationToPixelLine(15.0, 45.0, &srs_trad, &px, &line));

  OGRSpatialReference srs_auth;
  srs_auth.importFromEPSG(4326);
  srs_auth.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
  EXPECT_EQ(CE_None,
            ds.GeolocationToPixelLine(45.0, 15.0, &srs_auth, &px, &line));

  OGRSpatialReference srs_custom;
  srs_custom.SetWellKnownGeogCS("WGS84");
  srs_custom.SetDataAxisToSRSAxisMapping({1, 2});
  EXPECT_EQ(CE_None,
            ds.GeolocationToPixelLine(15.0, 45.0, &srs_custom, &px, &line));

  // GetExtent and GetExtentWGS84LongLat on raster dataset.
  EXPECT_EQ(CE_None, GDALGetExtent(hDS, &env, nullptr));
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(20.0, env.MaxX);
  EXPECT_EQ(CE_None, GDALGetExtentWGS84LongLat(hDS, &env));

  // GetExtent and TestCapability(GDsCFastGetExtent) on vector dataset.
  ExposedDataset vec_ds;
  EXPECT_EQ(TRUE, vec_ds.TestCapability(GDsCFastGetExtent));
  auto vl1 = std::make_unique<CustomLayer>("vl1", wkbPoint, &srs_trad);
  vl1->extent_.MinX = 1.0;
  vl1->extent_.MaxX = 5.0;
  vl1->extent_.MinY = 2.0;
  vl1->extent_.MaxY = 6.0;
  CustomLayer* vl1_ptr = vl1.get();
  vec_ds.layers_.push_back(std::move(vl1));
  EXPECT_EQ(TRUE, vec_ds.TestCapability(GDsCFastGetExtent));
  vl1_ptr->fast_get_extent_ = false;
  EXPECT_EQ(FALSE, vec_ds.TestCapability(GDsCFastGetExtent));
  EXPECT_EQ(CE_None, vec_ds.GetExtent(&env, nullptr));
  EXPECT_DOUBLE_EQ(1.0, env.MinX);
  EXPECT_DOUBLE_EQ(6.0, env.MaxY);

  // BuildFilename (requires GTiff driver registration for GTIFF_DIR:
  // subdatasets).
  GDALRegister_GTiff();
  struct BuildFilenameCase {
    const char* psz_filename;
    const char* psz_ref_dir;
    bool relative_to_ref_dir;
    std::string_view expected;
  };
  constexpr BuildFilenameCase kBuildFilenameCases[] = {
      {.psz_filename = "foo.tif",
       .psz_ref_dir = "/ref/dir",
       .relative_to_ref_dir = false,
       .expected = "foo.tif"},
      {.psz_filename = "../foo.tif",
       .psz_ref_dir = "/abc/def",
       .relative_to_ref_dir = true,
       .expected = "/abc/foo.tif"},
      {.psz_filename = "GTIFF_DIR:1:rel.tif",
       .psz_ref_dir = "/abc",
       .relative_to_ref_dir = true,
       .expected = "GTIFF_DIR:1:/abc/rel.tif"},
      {.psz_filename = "NITF_IM:0:rel.ntf",
       .psz_ref_dir = "/abc",
       .relative_to_ref_dir = true,
       .expected = "NITF_IM:0:/abc/rel.ntf"},
      {.psz_filename = "NITF_IM:0:z:/rel.ntf",
       .psz_ref_dir = "/abc",
       .relative_to_ref_dir = true,
       .expected = "NITF_IM:0:z:/rel.ntf"},
      {.psz_filename = "RASTERLITE:rel.db,table=t",
       .psz_ref_dir = "/abc",
       .relative_to_ref_dir = true,
       .expected = "RASTERLITE:/abc/rel.db,table=t"},
      {.psz_filename = "TILEDB:\"rel.tdb\":arr",
       .psz_ref_dir = "/abc",
       .relative_to_ref_dir = true,
       .expected = "TILEDB:\"/abc/rel.tdb\":arr"},
  };
  for (const auto& tc : kBuildFilenameCases) {
    EXPECT_EQ(tc.expected,
              GDALDataset::BuildFilename(tc.psz_filename, tc.psz_ref_dir,
                                         tc.relative_to_ref_dir));
  }
}

TEST(GdalDatasetTest, AsMDArrayComprehensive) {
  // 1. Error cases: 0 bands, non-uniform data types, invalid DIM_ORDER.
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    ExposedDataset ds_empty;
    EXPECT_EQ(nullptr, ds_empty.AsMDArray());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    ExposedDataset ds_mixed;
    ds_mixed.RasterInitialize(4, 4);
    ds_mixed.SetBand(1, new CustomBand(&ds_mixed, 1, GDT_Byte));
    ds_mixed.SetBand(2, new CustomBand(&ds_mixed, 2, GDT_UInt16));
    EXPECT_EQ(nullptr, ds_mixed.AsMDArray());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    ExposedDataset ds_bad_opt;
    ds_bad_opt.RasterInitialize(4, 4);
    ds_bad_opt.SetBand(1, new CustomBand(&ds_bad_opt, 1, GDT_Byte));
    constexpr std::array<const char*, 2> kBadDimOpts = {"DIM_ORDER=INVALID",
                                                        nullptr};
    EXPECT_EQ(nullptr, ds_bad_opt.AsMDArray(kBadDimOpts.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Int64 and UInt64 nodata datasets.
  {
    ExposedDatasetPtr ds_i64(std::make_unique<ExposedDataset>().release());
    ds_i64->RasterInitialize(4, 4);
    auto* b = new CustomBand(ds_i64.get(), 1, GDT_Int64);
    b->has_nodata_ = true;
    b->nodata_val_ = -999;
    ds_i64->SetBand(1, b);
    // SRS with North/East (EPSG:4326 authority compliant -> mapping {2, 1}).
    ds_i64->use_custom_srs_ = true;
    ds_i64->custom_srs_ = std::make_unique<OGRSpatialReference>();
    ds_i64->custom_srs_->importFromEPSG(4326);
    ds_i64->custom_srs_->SetDataAxisToSRSAxisMapping({2, 1});
    constexpr std::array<const char*, 2> kIdxOpts = {
        "BAND_INDEXING_VAR_ITEM={Index}", nullptr};
    auto md = ds_i64->AsMDArray(kIdxOpts.data());
    ASSERT_NE(nullptr, md);
    EXPECT_NE(nullptr, md->GetRawNoDataValue());
    EXPECT_NE(nullptr, md->GetSpatialRef());
  }

  {
    ExposedDatasetPtr ds_u64(std::make_unique<ExposedDataset>().release());
    ds_u64->RasterInitialize(4, 4);
    auto* b = new CustomBand(ds_u64.get(), 1, GDT_UInt64);
    b->has_nodata_ = true;
    b->nodata_val_ = 999;
    ds_u64->SetBand(1, b);
    // SRS with East/North (projected EPSG:32631 -> mapping {1, 2}).
    ds_u64->use_custom_srs_ = true;
    ds_u64->custom_srs_ = std::make_unique<OGRSpatialReference>();
    ds_u64->custom_srs_->importFromEPSG(32631);
    ds_u64->custom_srs_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    constexpr std::array<const char*, 3> kCiOpts = {
        "BAND_INDEXING_VAR_ITEM={ColorInterpretation}", "DIM_ORDER=Y,X,Band",
        nullptr};
    auto md = ds_u64->AsMDArray(kCiOpts.data());
    ASSERT_NE(nullptr, md);
    EXPECT_NE(nullptr, md->GetRawNoDataValue());
    EXPECT_NE(nullptr, md->GetSpatialRef());
    EXPECT_EQ(3U, md->GetBlockSize().size());
  }

  // 3. Multi-band Byte dataset testing BAND_INDEXING_VAR_ITEM / TYPE options,
  // GeoTransform indexing vars, Offset/Scale/Unit/Attributes, Read & Write.
  ExposedDatasetPtr ds_mb(std::make_unique<ExposedDataset>().release());
  ds_mb->eAccess = GA_Update;
  ds_mb->SetDescription("/vsimem/gdaldataset_test/md_test.bin");
  ds_mb->SetMetadataItem("ATTR1", "VAL1");
  ds_mb->RasterInitialize(4, 4);
  ds_mb->has_custom_gt_ = true;
  ds_mb->custom_gt_ = GDALGeoTransform(0.0, 1.0, 0.0, 10.0, 0.0, -1.0);
  ds_mb->use_custom_srs_ = true;
  ds_mb->custom_srs_ = std::make_unique<OGRSpatialReference>();
  ds_mb->custom_srs_->SetWellKnownGeogCS("WGS84");

  auto* mb1 = new CustomBand(ds_mb.get(), 1, GDT_Byte, 2, 2);
  mb1->unit_ = "m";
  mb1->has_nodata_ = true;
  mb1->nodata_val_ = 255.0;
  mb1->has_offset_ = true;
  mb1->offset_val_ = 1.5;
  mb1->has_scale_ = true;
  mb1->scale_val_ = 2.5;
  mb1->SetMetadataItem("WAVELENGTH", "550");

  auto* mb2 = new CustomBand(ds_mb.get(), 2, GDT_Byte, 2, 2);
  mb2->unit_ = "m";
  mb2->has_nodata_ = true;
  mb2->nodata_val_ = 255.0;
  mb2->has_offset_ = true;
  mb2->offset_val_ = 1.5;
  mb2->has_scale_ = true;
  mb2->scale_val_ = 2.5;
  mb2->SetMetadataItem("WAVELENGTH", "650");

  ds_mb->SetBand(1, mb1);
  ds_mb->SetBand(2, mb2);

  // Default ({Description}).
  {
    auto md = ds_mb->AsMDArray(nullptr);
    ASSERT_NE(nullptr, md);
    EXPECT_TRUE(md->IsWritable());
    EXPECT_EQ("/vsimem/gdaldataset_test/md_test.bin", md->GetFilename());
    EXPECT_EQ(3U, md->GetDimensions().size());
    EXPECT_EQ(GDT_Byte, md->GetDataType().GetNumericDataType());
    EXPECT_EQ("m", md->GetUnit());
    EXPECT_NE(nullptr, md->GetRawNoDataValue());
    bool has_val = false;
    GDALDataType st_dt = GDT_Byte;
    EXPECT_DOUBLE_EQ(1.5, md->GetOffset(&has_val, &st_dt));
    EXPECT_TRUE(has_val);
    EXPECT_DOUBLE_EQ(2.5, md->GetScale(&has_val, &st_dt));
    EXPECT_TRUE(has_val);
    EXPECT_NE(nullptr, md->GetSpatialRef());
    EXPECT_EQ(3U, md->GetBlockSize().size());
    EXPECT_EQ(1U, md->GetAttributes(nullptr).size());

    // Read and write with positive steps.
    constexpr std::array<GUInt64, 3> kStart = {0, 0, 0};
    constexpr std::array<size_t, 3> kCount = {2, 2, 2};
    constexpr std::array<GInt64, 3> kStep = {1, 1, 1};
    constexpr std::array<GPtrDiff_t, 3> kStride = {4, 2, 1};
    std::vector<GByte> buf(8, 0);
    EXPECT_TRUE(md->Read(kStart.data(), kCount.data(), kStep.data(),
                         kStride.data(), md->GetDataType(), buf.data()));
    EXPECT_TRUE(md->Write(kStart.data(), kCount.data(), kStep.data(),
                          kStride.data(), md->GetDataType(), buf.data()));

    // Read with negative steps in Y and X.
    constexpr std::array<GUInt64, 3> kStartNeg = {0, 1, 1};
    constexpr std::array<GInt64, 3> kStepNeg = {1, -1, -1};
    EXPECT_TRUE(md->Read(kStartNeg.data(), kCount.data(), kStepNeg.data(),
                         kStride.data(), md->GetDataType(), buf.data()));
  }

  // Custom metadata item with String, Integer, Real, and {None}.
  constexpr std::array<const char*, 3> kVarTypes = {"String", "Integer",
                                                    "Real"};
  for (const char* type_name : kVarTypes) {
    CPLStringList opts;
    opts.SetNameValue("BAND_INDEXING_VAR_ITEM", "WAVELENGTH");
    opts.SetNameValue("BAND_INDEXING_VAR_TYPE", type_name);
    EXPECT_NE(nullptr, ds_mb->AsMDArray(opts.List()));
  }
  CPLStringList none_opts;
  none_opts.SetNameValue("BAND_INDEXING_VAR_ITEM", "{None}");
  EXPECT_NE(nullptr, ds_mb->AsMDArray(none_opts.List()));
}

}  // namespace
}  // namespace autotest2
