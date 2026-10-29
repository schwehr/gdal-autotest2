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

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalantirecursion.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_featurestyle.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogr_attrind.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_multiproc.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

// Global functions defined in gdaldataset.cpp.
CPLMutex** GDALGetphDLMutex();
void GDALSetResponsiblePIDForCurrentThread(GIntBig responsiblePID);
GIntBig GDALGetResponsiblePIDForCurrentThread();
void GDALNullifyOpenDatasetsList();

namespace {

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
    int bytes = GDALGetDataTypeSizeBytes(eDataType) * nBlockXSize * nBlockYSize;
    memset(buf, nBand, bytes);
    return read_err_;
  }

  CPLErr IWriteBlock(int /*x*/, int /*y*/, void* /*buf*/) override {
    return write_err_;
  }

  CPLErr FlushCache(bool bAtClosing) override {
    if (flush_fail_) return CE_Failure;
    return GDALRasterBand::FlushCache(bAtClosing);
  }

  CPLErr DropCache() override {
    if (drop_fail_) return CE_Failure;
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
    if (custom_mask_flags_ >= 0) return custom_mask_flags_;
    return GDALRasterBand::GetMaskFlags();
  }

  GDALRasterBand* GetMaskBand() override {
    if (custom_mask_band_) return custom_mask_band_;
    return GDALRasterBand::GetMaskBand();
  }

  const char* GetUnitType() override { return unit_.c_str(); }

  double GetNoDataValue(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_nodata_ ? 1 : 0;
    return nodata_val_;
  }

  int64_t GetNoDataValueAsInt64(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_nodata_ ? 1 : 0;
    return static_cast<int64_t>(nodata_val_);
  }

  uint64_t GetNoDataValueAsUInt64(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_nodata_ ? 1 : 0;
    return static_cast<uint64_t>(nodata_val_);
  }

  double GetOffset(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_offset_ ? 1 : 0;
    return offset_val_;
  }

  double GetScale(int* pbSuccess) override {
    if (pbSuccess) *pbSuccess = has_scale_ ? 1 : 0;
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
    if (gtype != wkbNone && defn_->GetGeomFieldCount() > 0 && srs) {
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
    if (!m_poAttrIndex) {
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
    if (EQUAL(cap, OLCFastFeatureCount)) return fast_feature_count_ ? 1 : 0;
    if (EQUAL(cap, OLCFastGetExtent)) return fast_get_extent_ ? 1 : 0;
    if (EQUAL(cap, OLCTransactions)) return support_transactions_ ? 1 : 0;
    if (EQUAL(cap, OLCCurveGeometries)) return support_curves_ ? 1 : 0;
    return 0;
  }

  GIntBig GetFeatureCount(int /*bForce*/) override {
    if (neg_feature_count_) return -1;
    return static_cast<GIntBig>(features_.size());
  }

  OGRErr IGetExtent(int /*iGeomField*/, OGREnvelope* psExtent,
                    bool /*bForce*/) override {
    if (!has_extent_) return OGRERR_FAILURE;
    *psExtent = extent_;
    return OGRERR_NONE;
  }

  OGRErr SyncToDisk() override { return sync_err_; }

  OGRErr CreateField(const OGRFieldDefn* field, int /*bApproxOK*/) override {
    if (create_field_err_ != OGRERR_NONE) return create_field_err_;
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
    if (!f) return OGRERR_FAILURE;
    if (nFlagsIn & ALTER_NAME_FLAG) f->SetName(poNewFieldDefn->GetNameRef());
    if (nFlagsIn & ALTER_TYPE_FLAG) f->SetType(poNewFieldDefn->GetType());
    if (nFlagsIn & ALTER_WIDTH_PRECISION_FLAG) {
      f->SetWidth(poNewFieldDefn->GetWidth());
      f->SetPrecision(poNewFieldDefn->GetPrecision());
    }
    return OGRERR_NONE;
  }

  OGRErr CreateGeomField(const OGRGeomFieldDefn* field,
                         int /*bApproxOK*/) override {
    OGRGeomFieldDefn copy(field);
    if (copy.GetSpatialRef()) {
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
    if (create_feature_err_ != OGRERR_NONE) return create_feature_err_;
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
  std::vector<std::unique_ptr<OGRFeature>> features_;
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
    if (use_custom_srs_) return custom_srs_.get();
    return GDALDataset::GetSpatialRef();
  }

  const OGRSpatialReference* GetGCPSpatialRef() const override {
    if (use_custom_gcp_srs_) return custom_gcp_srs_.get();
    return GDALDataset::GetGCPSpatialRef();
  }

  using GDALDataset::GetGeoTransform;
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
    if (!allow_delete_layer_) return GDALDataset::DeleteLayer(i);
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
    OGRwkbGeometryType gtype =
        geom_field_defn ? geom_field_defn->GetType() : wkbNone;
    const OGRSpatialReference* srs =
        geom_field_defn ? geom_field_defn->GetSpatialRef() : nullptr;
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
    if (EQUAL(cap, ODsCCreateLayer)) return allow_create_layer_ ? 1 : 0;
    if (EQUAL(cap, ODsCDeleteLayer)) return allow_delete_layer_ ? 1 : 0;
    if (EQUAL(cap, ODsCCreateGeomFieldAfterCreateLayer))
      return allow_create_geom_field_ ? 1 : 0;
    if (EQUAL(cap, ODsCCurveGeometries)) return allow_curves_ ? 1 : 0;
    return GDALDataset::TestCapability(cap);
  }

  std::shared_ptr<GDALGroup> GetRootGroup() const override {
    if (root_group_) return root_group_;
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

void WriteMemFile(const char* path, const char* data = "x") {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(data, 1, strlen(data), fp);
  VSIFCloseL(fp);
}

TEST(GdalDatasetTest, LifecycleResponsiblePidCloseAndSuppressOnClose) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_NE(GDALGetphDLMutex(), nullptr);
  GIntBig orig_pid = CPLGetPID();
  EXPECT_EQ(GDALGetResponsiblePIDForCurrentThread(), orig_pid);
  GDALSetResponsiblePIDForCurrentThread(orig_pid + 42);
  EXPECT_EQ(GDALGetResponsiblePIDForCurrentThread(), orig_pid + 42);
  GDALSetResponsiblePIDForCurrentThread(orig_pid);

  // Test GDALDataset(int) constructor and Close / SuppressOnClose
  const char* tmp_file = "/vsimem/suppress_test.bin";
  WriteMemFile(tmp_file);
  {
    ExposedDataset ds(1);
    EXPECT_TRUE(ds.bForceCachedIO);
    ds.SetDescription(tmp_file);
    EXPECT_FALSE(ds.IsMarkedSuppressOnClose());
    GDALDatasetMarkSuppressOnClose(nullptr);
    GDALDatasetMarkSuppressOnClose(GDALDataset::ToHandle(&ds));
    EXPECT_TRUE(ds.IsMarkedSuppressOnClose());
    ds.UnMarkSuppressOnClose();
    EXPECT_FALSE(ds.IsMarkedSuppressOnClose());
    ds.MarkSuppressOnClose();
    EXPECT_EQ(GDALDatasetRunCloseWithoutDestroying(nullptr), CE_Failure);
    EXPECT_EQ(GDALDatasetRunCloseWithoutDestroying(GDALDataset::ToHandle(&ds)),
              CE_None);
    // File was unlinked and suppress flag cleared
    EXPECT_FALSE(ds.IsMarkedSuppressOnClose());
    VSIStatBufL stat_buf;
    EXPECT_NE(VSIStatL(tmp_file, &stat_buf), 0);
  }

  // Test CleanupPostFileClosing
  const char* cleanup_file = "/vsimem/cleanup_post.bin";
  WriteMemFile(cleanup_file);
  {
    ExposedDataset ds;
    ds.SetDescription(cleanup_file);
    ds.MarkSuppressOnClose();
    ds.CleanupPostFileClosing();
    VSIStatBufL stat_buf;
    EXPECT_NE(VSIStatL(cleanup_file, &stat_buf), 0);
  }

  // Test destructor with responsiblePID != CPLGetPID() on non-internal dataset
  const char* tif_path = "/vsimem/resp_pid.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(gtiff_drv, nullptr);
  GDALDataset* tif_ds = gtiff_drv->Create(tif_path, 4, 4, 1, GDT_Byte, nullptr);
  ASSERT_NE(tif_ds, nullptr);
  GDALSetResponsiblePIDForCurrentThread(orig_pid + 99);
  GDALClose(tif_ds);
  GDALSetResponsiblePIDForCurrentThread(orig_pid);
  VSIUnlink(tif_path);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, FlushCacheDropCacheAndBlockBasedFlushCache) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALFlushCache(nullptr), CE_Failure);
  EXPECT_EQ(GDALDropCache(nullptr), CE_Failure);

  ExposedDataset ds;
  ds.RasterInitialize(8, 8);
  EXPECT_EQ(ds.GetEstimatedRAMUsage(), -1);

  // Empty dataset BlockBasedFlushCache
  EXPECT_EQ(ds.BlockBasedFlushCache(false), CE_None);

  auto* b1 = new CustomBand(&ds, 1, GDT_Byte, 4, 4);
  auto* b2 = new CustomBand(&ds, 2, GDT_Byte, 4, 4);
  ds.SetBand(1, b1);
  ds.SetBand(2, std::unique_ptr<GDALRasterBand>(b2));

  auto lyr = std::make_unique<CustomLayer>("l1");
  CustomLayer* lyr_ptr = lyr.get();
  ds.layers_.push_back(std::move(lyr));

  EXPECT_EQ(GDALFlushCache(GDALDataset::ToHandle(&ds)), CE_None);
  EXPECT_EQ(GDALDropCache(GDALDataset::ToHandle(&ds)), CE_None);

  // Failing band FlushCache / DropCache and layer SyncToDisk
  b1->flush_fail_ = true;
  lyr_ptr->sync_err_ = OGRERR_FAILURE;
  EXPECT_EQ(ds.FlushCache(false), CE_Failure);
  b1->flush_fail_ = false;
  lyr_ptr->sync_err_ = OGRERR_NONE;

  b2->drop_fail_ = true;
  EXPECT_EQ(ds.DropCache(), CE_Failure);
  b2->drop_fail_ = false;

  // BlockBasedFlushCache with matching block sizes
  EXPECT_EQ(ds.BlockBasedFlushCache(false), CE_None);

  // Dirty a block in b1 and make IWriteBlock fail during BlockBasedFlushCache
  ds.eAccess = GA_Update;
  b1->eAccess = GA_Update;
  GDALRasterBlock* blk = b1->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(blk, nullptr);
  blk->MarkDirty();
  blk->DropLock();
  b1->write_err_ = CE_Failure;
  EXPECT_EQ(ds.BlockBasedFlushCache(false), CE_Failure);
  b1->write_err_ = CE_None;

  // BlockBasedFlushCache when marked suppress on close & bAtClosing == true
  ds.MarkSuppressOnClose();
  EXPECT_EQ(ds.BlockBasedFlushCache(true), CE_None);
  ds.UnMarkSuppressOnClose();

  // BlockBasedFlushCache when band 3 has different block X and Y sizes
  auto* b3 = new CustomBand(&ds, 3, GDT_Byte, 2, 2);
  ds.SetBand(3, b3);
  EXPECT_EQ(ds.BlockBasedFlushCache(false), CE_None);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, BandsRasterSizeAndSetBandEdgeCases) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALGetRasterXSize(nullptr), 0);
  EXPECT_EQ(GDALGetRasterYSize(nullptr), 0);
  EXPECT_EQ(GDALGetRasterCount(nullptr), 0);
  EXPECT_EQ(GDALGetRasterBand(nullptr, 1), nullptr);
  EXPECT_EQ(GDALAddBand(nullptr, GDT_Byte, nullptr), CE_Failure);

  ExposedDataset ds;
  ds.RasterInitialize(16, 12);
  const ExposedDataset& const_ds = ds;

  EXPECT_EQ(GDALGetRasterXSize(GDALDataset::ToHandle(&ds)), 16);
  EXPECT_EQ(GDALGetRasterYSize(GDALDataset::ToHandle(&ds)), 12);
  EXPECT_EQ(GDALGetRasterCount(GDALDataset::ToHandle(&ds)), 0);
  EXPECT_EQ(ds.GetRasterBand(1), nullptr);
  EXPECT_EQ(const_ds.GetRasterBand(1), nullptr);
  EXPECT_EQ(GDALAddBand(GDALDataset::ToHandle(&ds), GDT_Byte, nullptr),
            CE_Failure);

  // SetBand skipping band 1 (sets band 2, leaving band 1 as nullptr)
  auto* b2 = new CustomBand(&ds, 2);
  ds.SetBand(2, b2);
  EXPECT_EQ(ds.GetRasterCount(), 2);
  EXPECT_EQ(ds.GetRasterBand(1), nullptr);
  EXPECT_EQ(ds.GetRasterBand(2), b2);
  EXPECT_EQ(const_ds.GetRasterBand(2), b2);

  // Out of bounds GetRasterBand (const and non-const)
  EXPECT_EQ(ds.GetRasterBand(0), nullptr);
  EXPECT_EQ(ds.GetRasterBand(3), nullptr);
  EXPECT_EQ(const_ds.GetRasterBand(0), nullptr);
  EXPECT_EQ(const_ds.GetRasterBand(3), nullptr);

  // Setting already-set band 2 fails
  CustomBand duplicate_band(&ds, 2);
  ds.SetBand(2, &duplicate_band);
  EXPECT_EQ(ds.GetRasterBand(2), b2);

  // Set band 1 properly
  auto* b1 = new CustomBand(&ds, 1);
  ds.SetBand(1, b1);

  // Test GetBands() and ConstBands iterators and indexing
  auto bands = ds.GetBands();
  EXPECT_EQ(bands.size(), 2U);
  EXPECT_EQ(bands[0], b1);
  EXPECT_EQ(bands[static_cast<size_t>(1)], b2);
  int count = 0;
  for (GDALRasterBand* b : bands) {
    EXPECT_NE(b, nullptr);
    ++count;
  }
  EXPECT_EQ(count, 2);

  auto const_bands = const_ds.GetBands();
  EXPECT_EQ(const_bands.size(), 2U);
  EXPECT_EQ(const_bands[0], b1);
  EXPECT_EQ(const_bands[static_cast<size_t>(1)], b2);
  count = 0;
  for (const GDALRasterBand* b : const_bands) {
    EXPECT_NE(b, nullptr);
    ++count;
  }
  EXPECT_EQ(count, 2);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, SpatialRefGeoTransformAndGCPs) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALGetSpatialRef(nullptr), nullptr);
  EXPECT_EQ(GDALGetProjectionRef(nullptr), nullptr);
  EXPECT_EQ(GDALSetSpatialRef(nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetProjection(nullptr, "EPSG:4326"), CE_Failure);
  EXPECT_EQ(GDALGetGeoTransform(nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetGeoTransform(nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALGetGCPCount(nullptr), 0);
  EXPECT_EQ(GDALGetGCPSpatialRef(nullptr), nullptr);
  EXPECT_EQ(GDALGetGCPProjection(nullptr), nullptr);
  EXPECT_EQ(GDALGetGCPs(nullptr), nullptr);
  EXPECT_EQ(GDALSetGCPs(nullptr, 0, nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetGCPs2(nullptr, 0, nullptr, nullptr), CE_Failure);

  ExposedDataset ds;
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // Default SpatialRef / ProjectionRef
  EXPECT_EQ(GDALGetSpatialRef(hDS), nullptr);
  EXPECT_STREQ(GDALGetProjectionRef(hDS), "");
  EXPECT_EQ(ds.GetSpatialRefRasterOnly(), nullptr);

  // SetProjection / SetSpatialRef on base class
  EXPECT_EQ(GDALSetProjection(hDS, ""), CE_Failure);
  EXPECT_EQ(GDALSetProjection(hDS, "INVALID_SRS_STRING"), CE_Failure);
  EXPECT_EQ(GDALSetProjection(hDS, "EPSG:4326"), CE_Failure);
  ds.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetSpatialRef(hDS, nullptr), CE_Failure);
  ds.SetMOFlags(0);

  // Custom SRS caching in GetProjectionRef
  ds.use_custom_srs_ = true;
  ds.custom_srs_ = std::make_unique<OGRSpatialReference>();
  ds.custom_srs_->SetWellKnownGeogCS("WGS84");
  const char* wkt1 = ds.GetProjectionRef();
  EXPECT_NE(wkt1[0], '\0');
  const char* wkt2 = ds.GetProjectionRef();
  EXPECT_EQ(wkt1, wkt2);  // Cached pointer returned
  ds.custom_srs_->SetWellKnownGeogCS("NAD27");
  const char* wkt3 = ds.GetProjectionRef();
  EXPECT_NE(wkt3[0], '\0');
  ds.use_custom_srs_ = false;

  // GetSpatialRefVectorOnly with layers
  OGRSpatialReference srs1;
  srs1.SetWellKnownGeogCS("WGS84");
  OGRSpatialReference srs2;
  srs2.SetWellKnownGeogCS("NAD27");

  ds.layers_.push_back(std::make_unique<CustomLayer>("l1", wkbPoint, &srs1));
  EXPECT_NE(ds.GetSpatialRef(), nullptr);
  ds.layers_.push_back(std::make_unique<CustomLayer>("l2", wkbPoint, &srs1));
  EXPECT_NE(ds.GetSpatialRef(), nullptr);
  // Layer with different SRS -> returns nullptr
  ds.layers_.push_back(std::make_unique<CustomLayer>("l3", wkbPoint, &srs2));
  EXPECT_EQ(ds.GetSpatialRef(), nullptr);
  ds.layers_.clear();

  // GeoTransform
  double adfGT[6] = {0};
  EXPECT_EQ(ds.GetGeoTransform(adfGT), CE_Failure);
  EXPECT_EQ(GDALGetGeoTransform(hDS, adfGT), CE_Failure);
  EXPECT_DOUBLE_EQ(adfGT[1], 1.0);
  EXPECT_EQ(GDALSetGeoTransform(hDS, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetGeoTransform(hDS, adfGT), CE_Failure);
  EXPECT_EQ(ds.SetGeoTransform(adfGT), CE_Failure);
  ds.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(ds.SetGeoTransform(adfGT), CE_Failure);
  ds.SetMOFlags(0);

  // GCPs
  EXPECT_EQ(GDALGetGCPCount(hDS), 0);
  EXPECT_EQ(GDALGetGCPSpatialRef(hDS), nullptr);
  EXPECT_STREQ(GDALGetGCPProjection(hDS), "");
  EXPECT_EQ(GDALGetGCPs(hDS), nullptr);
  EXPECT_EQ(GDALSetGCPs(hDS, 0, nullptr, ""), CE_Failure);
  EXPECT_EQ(GDALSetGCPs(hDS, 0, nullptr, "INVALID_WKT"), CE_Failure);
  char* srs_wkt = nullptr;
  srs1.exportToWkt(&srs_wkt);
  EXPECT_EQ(GDALSetGCPs(hDS, 0, nullptr, srs_wkt), CE_Failure);
  CPLFree(srs_wkt);
  EXPECT_EQ(GDALSetGCPs2(hDS, 0, nullptr, OGRSpatialReference::ToHandle(&srs1)),
            CE_Failure);
  ds.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetGCPs2(hDS, 0, nullptr, nullptr), CE_Failure);
  ds.SetMOFlags(0);

  // Custom GCP SRS caching in GetGCPProjection
  ds.use_custom_gcp_srs_ = true;
  ds.custom_gcp_srs_ = std::make_unique<OGRSpatialReference>();
  ds.custom_gcp_srs_->SetWellKnownGeogCS("WGS84");
  const char* gcp_wkt1 = ds.GetGCPProjection();
  EXPECT_NE(gcp_wkt1[0], '\0');
  const char* gcp_wkt2 = ds.GetGCPProjection();
  EXPECT_EQ(gcp_wkt1, gcp_wkt2);
  ds.custom_gcp_srs_->SetWellKnownGeogCS("NAD27");
  EXPECT_NE(ds.GetGCPProjection()[0], '\0');

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, InternalHandleDriverRefCountsAndSharedDatasets) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALGetInternalHandle(nullptr, "foo"), nullptr);
  EXPECT_EQ(GDALGetDatasetDriver(nullptr), nullptr);
  EXPECT_EQ(GDALReferenceDataset(nullptr), 0);
  EXPECT_EQ(GDALDereferenceDataset(nullptr), 0);
  EXPECT_EQ(GDALReleaseDataset(nullptr), 0);
  EXPECT_EQ(GDALGetAccess(nullptr), 0);
  GDALGetOpenDatasets(nullptr, nullptr);

  auto* heap_ds = new ExposedDataset();
  GDALDatasetH hDS = GDALDataset::ToHandle(heap_ds);
  EXPECT_EQ(GDALGetInternalHandle(hDS, "foo"), nullptr);
  EXPECT_EQ(GDALGetDatasetDriver(hDS), nullptr);
  EXPECT_STREQ(heap_ds->GetDriverName(), "");
  EXPECT_EQ(GDALGetAccess(hDS), GA_ReadOnly);

  EXPECT_EQ(heap_ds->GetRefCount(), 1);
  EXPECT_EQ(GDALReferenceDataset(hDS), 2);
  EXPECT_EQ(heap_ds->GetSummaryRefCount(), 2);

  // Add a layer and check GetSummaryRefCount
  auto lyr = std::make_unique<CustomLayer>("l1");
  lyr->Reference();
  heap_ds->layers_.push_back(std::move(lyr));
  EXPECT_EQ(heap_ds->GetSummaryRefCount(), 3);

  // ReleaseRef when refcount > 1 returns FALSE
  EXPECT_EQ(GDALReleaseDataset(hDS), FALSE);
  EXPECT_EQ(heap_ds->GetRefCount(), 1);
  EXPECT_EQ(GDALReferenceDataset(hDS), 2);
  EXPECT_EQ(GDALDereferenceDataset(hDS), 1);

  // MarkAsShared on internal dataset
  EXPECT_EQ(heap_ds->GetShared(), FALSE);
  heap_ds->MarkAsShared();
  EXPECT_EQ(heap_ds->GetShared(), TRUE);

  // Release() destroys when refcount drops to 0
  EXPECT_EQ(heap_ds->Release(), OGRERR_NONE);

  // Test duplicate MarkAsShared on two non-internal datasets with same name
  const char* gtiff_file1 = "/vsimem/shared_dup1.tif";
  const char* gtiff_file2 = "/vsimem/shared_dup2.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(gtiff_drv, nullptr);
  GDALDataset* ds1 = gtiff_drv->Create(gtiff_file1, 4, 4, 1, GDT_Byte, nullptr);
  GDALDataset* ds2 = gtiff_drv->Create(gtiff_file2, 4, 4, 1, GDT_Byte, nullptr);
  ASSERT_NE(ds1, nullptr);
  ASSERT_NE(ds2, nullptr);
  ds1->MarkAsShared();
  ds2->SetDescription(gtiff_file1);
  ds2->MarkAsShared();  // Duplicate description in phSharedDatasetSet!
  // Change ds1's description so UnregisterFromSharedDataset hits the 'else'
  // branch
  ds1->SetDescription("/vsimem/changed_desc.tif");
  GDALClose(ds1);
  ds2->SetDescription(gtiff_file2);
  GDALClose(ds2);
  VSIUnlink(gtiff_file1);
  VSIUnlink(gtiff_file2);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, BuildOverviewsAddOverviewsFileListAndMaskBand) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALBuildOverviews(nullptr, "NEAREST", 0, nullptr, 0, nullptr,
                               nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALBuildOverviewsEx(nullptr, "NEAREST", 0, nullptr, 0, nullptr,
                                 nullptr, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetFileList(nullptr), nullptr);
  EXPECT_EQ(GDALCreateDatasetMaskBand(nullptr, GMF_PER_DATASET), CE_Failure);

  // Uninitialized oOvManager dataset
  ExposedDataset ds;
  ds.RasterInitialize(8, 8);
  ds.SetBand(1, new CustomBand(&ds, 1));

  // Fake driver with GDAL_DMD_OVERVIEW_CREATIONOPTIONLIST metadata
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
  EXPECT_STREQ(ds.GetDriverName(), "FAKE_DRV");

  const char* ovr_opts[] = {"USE_RRD=YES", "VRT_VIRTUAL_OVERVIEWS=YES",
                            "COMPRESS_OVERVIEW=DEFLATE", nullptr};
  int bad_ovr_list[] = {-2};
  EXPECT_EQ(GDALBuildOverviewsEx(GDALDataset::ToHandle(&ds), "NEAREST", 1,
                                 bad_ovr_list, 0, nullptr, nullptr, nullptr,
                                 ovr_opts),
            CE_Failure);

  int good_ovr_list[] = {2};
  // Fails because oOvManager is not initialized
  EXPECT_EQ(GDALBuildOverviews(GDALDataset::ToHandle(&ds), "NEAREST", 1,
                               good_ovr_list, 0, nullptr, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(ds.AddOverviews({}, nullptr, nullptr, nullptr), CE_Failure);
  EXPECT_EQ(
      GDALCreateDatasetMaskBand(GDALDataset::ToHandle(&ds), GMF_PER_DATASET),
      CE_Failure);

  // Now test on a real GTiff in /vsimem/ with external overview and mask
  const char* tif_path = "/vsimem/ovr_mask_test.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(gtiff_drv, nullptr);
  GDALDataset* tif_ds =
      gtiff_drv->Create(tif_path, 16, 16, 1, GDT_Byte, nullptr);
  ASSERT_NE(tif_ds, nullptr);
  GDALClose(tif_ds);

  // Open read-only so BuildOverviews creates an external .ovr file
  tif_ds = GDALDataset::FromHandle(GDALOpen(tif_path, GA_ReadOnly));
  ASSERT_NE(tif_ds, nullptr);
  EXPECT_EQ(tif_ds->BuildOverviews("NEAREST", 1, good_ovr_list, 0, nullptr,
                                   nullptr, nullptr, nullptr),
            CE_None);
  {
    CPLConfigOptionSetter msk_setter("GDAL_TIFF_INTERNAL_MASK", "NO", false);
    EXPECT_EQ(GDALCreateDatasetMaskBand(GDALDataset::ToHandle(tif_ds),
                                        GMF_PER_DATASET),
              CE_None);
  }
  char** file_list = GDALGetFileList(GDALDataset::ToHandle(tif_ds));
  EXPECT_GE(CSLCount(file_list), 2);
  CSLDestroy(file_list);

  GDALClose(tif_ds);
  VSIUnlink(tif_path);
  VSIUnlink("/vsimem/ovr_mask_test.tif.ovr");
  VSIUnlink("/vsimem/ovr_mask_test.tif.msk");

  // Test GDALAntiRecursionGuard
  GDALAntiRecursionGuard g1("test_id");
  EXPECT_EQ(g1.GetCallDepth(), 1);
  GDALAntiRecursionGuard g2(g1, "_sub");
  EXPECT_EQ(g2.GetCallDepth(), 1);
  GDALAntiRecursionGuard g3(g1, "");
  EXPECT_EQ(g3.GetCallDepth(), 0);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, RasterIOAndAdviseReadComprehensive) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALDatasetRasterIO(nullptr, GF_Read, 0, 0, 1, 1, nullptr, 1, 1,
                                GDT_Byte, 1, nullptr, 0, 0, 0),
            CE_Failure);
  EXPECT_EQ(GDALDatasetRasterIOEx(nullptr, GF_Read, 0, 0, 1, 1, nullptr, 1, 1,
                                  GDT_Byte, 1, nullptr, 0, 0, 0, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALDatasetAdviseRead(nullptr, 0, 0, 1, 1, 1, 1, GDT_Byte, 1,
                                  nullptr, nullptr),
            CE_Failure);

  ExposedDataset ds;
  ds.RasterInitialize(8, 8);
  auto* b1 = new CustomBand(&ds, 1, GDT_Byte, 8, 1);
  auto* b2 = new CustomBand(&ds, 2, GDT_Byte, 8, 1);
  ds.SetBand(1, b1);
  ds.SetBand(2, b2);

  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);
  GByte buf[128] = {0};

  // ExtraArg version too high
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  extra.nVersion = RASTERIO_EXTRA_ARG_CURRENT_VERSION + 10;
  EXPECT_EQ(GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                  1, nullptr, 0, 0, 0, &extra),
            CE_Failure);
  INIT_RASTERIO_EXTRA_ARG(extra);

  // Null buffer
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, nullptr, 4, 4,
                                GDT_Byte, 1, nullptr, 0, 0, 0),
            CE_Failure);

  // GF_Write on read-only dataset
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Write, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                1, nullptr, 0, 0, 0),
            CE_Failure);

  // Noop window (size 0)
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 0, 4, buf, 4, 4, GDT_Byte,
                                1, nullptr, 0, 0, 0),
            CE_None);

  // Window out of range
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, -1, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                1, nullptr, 0, 0, 0),
            CE_Failure);
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 10, 4, buf, 4, 4, GDT_Byte,
                                1, nullptr, 0, 0, 0),
            CE_Failure);

  // Band count > GetRasterCount() with null band map
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                5, nullptr, 0, 0, 0),
            CE_Failure);

  // Invalid band in panBandMap
  int bad_map[] = {0};
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                1, bad_map, 0, 0, 0),
            CE_Failure);

  // Invalid eBufType
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4,
                                GDT_Unknown, 1, nullptr, 0, 0, 0),
            CE_Failure);

  // Valid multi-band read
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                2, nullptr, 0, 0, 0),
            CE_None);

  // Forced cached IO
  ds.bForceCachedIO = true;
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                2, nullptr, 0, 0, 0),
            CE_None);
  ds.bForceCachedIO = false;

  // Pixel interleaved metadata -> BlockBasedRasterIO
  ds.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  EXPECT_EQ(GDALDatasetRasterIO(hDS, GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                2, nullptr, 0, 0, 0),
            CE_None);
  ds.SetMetadataItem("INTERLEAVE", nullptr, "IMAGE_STRUCTURE");

  // Multi-band resampled read (GRIORA_Bilinear) with all bands OK
  extra.eResampleAlg = GRIORA_Bilinear;
  EXPECT_EQ(GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf, 4, 4, GDT_Byte,
                                  2, nullptr, 0, 0, 0, &extra),
            CE_None);

  // Multi-band resampled read where band 1 is OK and band 2 has GMF_NODATA
  // (exercises partial nOKBands == 1 < nBandCount == 2)
  b2->custom_mask_flags_ = GMF_NODATA;
  EXPECT_EQ(GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf, 4, 4, GDT_Byte,
                                  2, nullptr, 0, 0, 0, &extra),
            CE_None);
  b2->custom_mask_flags_ = -1;

  // Band 2 has a ColorTable
  b2->color_table_ = std::make_unique<GDALColorTable>();
  EXPECT_EQ(GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf, 4, 4, GDT_Byte,
                                  2, nullptr, 0, 0, 0, &extra),
            CE_None);
  b2->color_table_.reset();

  // Band 1 has overviews -> breaks out of loop at i==0 (nOKBands == 0)
  b1->ovr_count_ = 1;
  b1->overviews_.push_back(
      std::make_unique<CustomBand>(&ds, 1, GDT_Byte, 4, 1));
  b1->overviews_[0]->nRasterXSize = 4;
  b1->overviews_[0]->nRasterYSize = 4;
  EXPECT_EQ(GDALDatasetRasterIOEx(hDS, GF_Read, 0, 0, 8, 8, buf, 4, 4, GDT_Byte,
                                  2, nullptr, 0, 0, 0, &extra),
            CE_None);
  b1->ovr_count_ = 0;
  b1->overviews_.clear();

  // AdviseRead tests
  EXPECT_EQ(GDALDatasetAdviseRead(hDS, 0, 0, 0, 4, 4, 4, GDT_Byte, 2, nullptr,
                                  nullptr),
            CE_None);
  EXPECT_EQ(GDALDatasetAdviseRead(hDS, -1, 0, 4, 4, 4, 4, GDT_Byte, 2, nullptr,
                                  nullptr),
            CE_Failure);
  int map2[] = {1, 2};
  EXPECT_EQ(
      GDALDatasetAdviseRead(hDS, 0, 0, 4, 4, 4, 4, GDT_Byte, 2, map2, nullptr),
      CE_None);
  b2->advise_err_ = CE_Failure;
  EXPECT_EQ(
      GDALDatasetAdviseRead(hDS, 0, 0, 4, 4, 4, 4, GDT_Byte, 2, map2, nullptr),
      CE_Failure);
  b2->advise_err_ = CE_None;

  // Null band in middle of band array triggers error in
  // ValidateRasterIOOrAdviseReadParameters
  ExposedDataset sparse_ds;
  sparse_ds.RasterInitialize(4, 4);
  sparse_ds.SetBand(2, new CustomBand(&sparse_ds, 2));
  int map_band1[] = {1};
  EXPECT_EQ(sparse_ds.RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_Byte, 1,
                               map_band1, 0, 0, 0, nullptr),
            CE_Failure);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, OpenOpenExOpenSharedDumpAndAsyncReader) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALOpenEx(nullptr, 0, nullptr, nullptr, nullptr), nullptr);
  EXPECT_EQ(GDALOpenShared(nullptr, GA_ReadOnly), nullptr);
  EXPECT_EQ(GDALClose(nullptr), CE_None);
  EXPECT_EQ(GDALDumpOpenDatasets(nullptr), 0);

  // Incompatible GDAL_OF_THREAD_SAFE flags
  EXPECT_EQ(GDALOpenEx("/vsimem/no.tif",
                       GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_UPDATE,
                       nullptr, nullptr, nullptr),
            nullptr);
  EXPECT_EQ(GDALOpenEx("/vsimem/no.tif",
                       GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_VECTOR,
                       nullptr, nullptr, nullptr),
            nullptr);
  EXPECT_EQ(
      GDALOpenEx("/vsimem/no.tif",
                 GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_MULTIDIM_RASTER,
                 nullptr, nullptr, nullptr),
      nullptr);
  EXPECT_EQ(GDALOpenEx("/vsimem/no.tif",
                       GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_GNM,
                       nullptr, nullptr, nullptr),
            nullptr);

  // Incompatible GDAL_OF_SHARED | GDAL_OF_INTERNAL
  EXPECT_EQ(GDALOpenEx("/vsimem/no.tif", GDAL_OF_SHARED | GDAL_OF_INTERNAL,
                       nullptr, nullptr, nullptr),
            nullptr);

  // Non-existent file with GDAL_OF_VERBOSE_ERROR and CACHE_KERCHUNK_JSON
  const char* kerchunk_opts[] = {"CACHE_KERCHUNK_JSON=YES",
                                 "@VALIDATE_OPEN_OPTIONS=NO", nullptr};
  EXPECT_EQ(GDALOpenEx("/vsimem/does_not_exist_12345.tif",
                       GDAL_OF_VERBOSE_ERROR, nullptr, kerchunk_opts, nullptr),
            nullptr);

  // Existing unrecognized file with GDAL_OF_VERBOSE_ERROR
  const char* unrec_file = "/vsimem/unrecognized_file.bin";
  WriteMemFile(unrec_file, "NOT_A_VALID_FORMAT_HEADER");
  EXPECT_EQ(
      GDALOpenEx(unrec_file, GDAL_OF_VERBOSE_ERROR, nullptr, nullptr, nullptr),
      nullptr);
  VSIUnlink(unrec_file);

  // Create a real GTiff with overviews and test GDALOpenShared, OVERVIEW_LEVEL,
  // GDAL_OF_THREAD_SAFE, GDALGetOpenDatasets, GDALDumpOpenDatasets, AsyncReader
  const char* tif_file = "/vsimem/open_test.tif";
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(gtiff_drv, nullptr);
  GDALDataset* ds_create =
      gtiff_drv->Create(tif_file, 16, 16, 1, GDT_Byte, nullptr);
  ASSERT_NE(ds_create, nullptr);
  int ovr_factors[] = {2};
  EXPECT_EQ(ds_create->BuildOverviews("NEAREST", 1, ovr_factors, 0, nullptr,
                                      nullptr, nullptr, nullptr),
            CE_None);
  GDALClose(ds_create);

  // GDALOpenShared in update mode, then read-only mode (reuses update dataset!)
  GDALDatasetH hShared1 = GDALOpenShared(tif_file, GA_Update);
  ASSERT_NE(hShared1, nullptr);
  GDALDatasetH hShared2 = GDALOpenShared(tif_file, GA_ReadOnly);
  EXPECT_EQ(hShared1, hShared2);

  // Open non-shared dataset too so GDALDumpOpenDatasets sees both shared and
  // non-shared
  GDALDatasetH hNonShared = GDALOpen(tif_file, GA_ReadOnly);
  ASSERT_NE(hNonShared, nullptr);

  int open_count = 0;
  GDALDatasetH* open_list = nullptr;
  GDALGetOpenDatasets(&open_list, &open_count);
  EXPECT_GE(open_count, 2);

  FILE* tmp_fp = tmpfile();
  if (tmp_fp) {
    EXPECT_GE(GDALDumpOpenDatasets(tmp_fp), 2);
    fclose(tmp_fp);
  }

  // AsyncReader on hNonShared
  EXPECT_EQ(GDALBeginAsyncReader(nullptr, 0, 0, 4, 4, nullptr, 4, 4, GDT_Byte,
                                 1, nullptr, 0, 0, 0, nullptr),
            nullptr);
  GDALEndAsyncReader(nullptr, nullptr);
  GByte async_buf[16] = {0};
  GDALAsyncReaderH hAsync =
      GDALBeginAsyncReader(hNonShared, 0, 0, 4, 4, async_buf, 4, 4, GDT_Byte, 1,
                           nullptr, 0, 0, 0, nullptr);
  ASSERT_NE(hAsync, nullptr);
  GDALEndAsyncReader(hNonShared, hAsync);

  EXPECT_EQ(GDALClose(hNonShared), CE_None);
  EXPECT_EQ(GDALClose(hShared2), CE_None);  // Decrements refcount from 2 to 1
  EXPECT_EQ(GDALClose(hShared1), CE_None);  // Closes

  // Open with OVERVIEW_LEVEL=0, OVERVIEW_LEVEL=0only, OVERVIEW_LEVEL=NONE,
  // OVERVIEW_LEVEL=99
  const char* allowed_drivers[] = {"GTiff", nullptr};
  const char* ovr0_opts[] = {"OVERVIEW_LEVEL=0only", nullptr};
  GDALDatasetH hOvr0 = GDALOpenEx(tif_file, GDAL_OF_RASTER | GDAL_OF_SHARED,
                                  allowed_drivers, ovr0_opts, nullptr);
  ASSERT_NE(hOvr0, nullptr);
  EXPECT_EQ(GDALGetRasterXSize(hOvr0), 8);
  GDALClose(hOvr0);

  const char* ovr_none_opts[] = {"OVERVIEW_LEVEL=NONE", nullptr};
  GDALDataset* poOvrNone = GDALDataset::Open(
      tif_file, GDAL_OF_RASTER, allowed_drivers, ovr_none_opts, nullptr);
  ASSERT_NE(poOvrNone, nullptr);
  EXPECT_NE(poOvrNone->GetOpenOptions(), nullptr);
  EXPECT_NE(const_cast<const GDALDataset*>(poOvrNone)->GetOpenOptions(),
            nullptr);
  GDALClose(poOvrNone);

  const char* ovr_bad_opts[] = {"OVERVIEW_LEVEL=99", nullptr};
  EXPECT_EQ(GDALOpenEx(tif_file, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR,
                       allowed_drivers, ovr_bad_opts, nullptr),
            nullptr);

  // Open with GDAL_OF_THREAD_SAFE | GDAL_OF_RASTER | GDAL_OF_SHARED
  GDALDatasetH hThreadSafe = GDALOpenEx(
      tif_file, GDAL_OF_RASTER | GDAL_OF_THREAD_SAFE | GDAL_OF_SHARED,
      allowed_drivers, nullptr, nullptr);
  ASSERT_NE(hThreadSafe, nullptr);
  GDALClose(hThreadSafe);

  VSIUnlink(tif_file);
  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, ReportErrorAndDerivedSubdatasetsMetadata) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  ExposedDataset ds;
  ds.RasterInitialize(4, 4);
  ds.SetDescription("");
  ds.ReportError(CE_Warning, CPLE_AppDefined, "Empty desc warning %d", 1);
  ds.SetDescription("/vsimem/my_ds.tif");
  ds.ReportError(CE_Warning, CPLE_AppDefined, "Named desc warning %d", 2);
  GDALDataset::ReportError("/vsimem/static_ds.tif", CE_Warning, CPLE_AppDefined,
                           "Static warning %d", 3);
  GDALDataset::ReportUpdateNotSupportedByDriver("TESTDRV");

  // 0 bands -> no DERIVED_SUBDATASETS domain
  char** domains0 = ds.GetMetadataDomainList();
  EXPECT_EQ(CSLFindString(domains0, "DERIVED_SUBDATASETS"), -1);
  CSLDestroy(domains0);
  EXPECT_EQ(ds.GetMetadata("DERIVED_SUBDATASETS"), nullptr);

  // 1 real band
  ds.SetBand(1, new CustomBand(&ds, 1, GDT_Byte));
  char** domains1 = ds.GetMetadataDomainList();
  EXPECT_GE(CSLFindString(domains1, "DERIVED_SUBDATASETS"), 0);
  CSLDestroy(domains1);
  char** derived_md1 = ds.GetMetadata("DERIVED_SUBDATASETS");
  EXPECT_NE(derived_md1, nullptr);
  const int count1 = CSLCount(derived_md1);

  // Add a complex band -> includes complex derived subdatasets too
  ds.SetBand(2, new CustomBand(&ds, 2, GDT_CFloat32));
  char** derived_md2 = ds.GetMetadata("DERIVED_SUBDATASETS");
  EXPECT_GE(CSLCount(derived_md2), count1);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, VectorLayersCreateCopyAndIterators) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALDatasetGetLayerCount(nullptr), 0);
  EXPECT_EQ(GDALDatasetGetLayer(nullptr, 0), nullptr);
  EXPECT_EQ(GDALDatasetGetLayerByName(nullptr, "a"), nullptr);
  EXPECT_EQ(GDALDatasetIsLayerPrivate(nullptr, 0), 0);
  EXPECT_EQ(GDALDatasetDeleteLayer(nullptr, 0), OGRERR_INVALID_HANDLE);
  EXPECT_EQ(GDALDatasetCreateLayer(nullptr, "a", nullptr, wkbPoint, nullptr),
            nullptr);
  EXPECT_EQ(
      GDALDatasetCreateLayerFromGeomFieldDefn(nullptr, "a", nullptr, nullptr),
      nullptr);
  EXPECT_EQ(GDALDatasetCopyLayer(nullptr, nullptr, "a", nullptr), nullptr);

  ExposedDataset ds;
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // Base class defaults
  struct MinimalDataset : public GDALDataset {
  } base_ds;
  EXPECT_EQ(base_ds.GetLayerCount(), 0);
  EXPECT_EQ(base_ds.GetLayer(0), nullptr);
  EXPECT_FALSE(base_ds.IsLayerPrivate(0));
  EXPECT_EQ(base_ds.DeleteLayer(0), OGRERR_UNSUPPORTED_OPERATION);
  EXPECT_EQ(base_ds.CreateLayer("l1"), nullptr);
  EXPECT_EQ(base_ds.CreateLayer("l1", nullptr), nullptr);

  // Null layer name checks in C API
  EXPECT_EQ(GDALDatasetCreateLayer(hDS, nullptr, nullptr, wkbPoint, nullptr),
            nullptr);
  EXPECT_EQ(
      GDALDatasetCreateLayerFromGeomFieldDefn(hDS, nullptr, nullptr, nullptr),
      nullptr);

  // CopyLayer when ODsCCreateLayer is false
  CustomLayer src_lyr("src", wkbCurvePolygon);
  EXPECT_EQ(ds.CopyLayer(&src_lyr, "dst", nullptr), nullptr);

  // Enable layer creation on ds
  ds.allow_create_layer_ = true;
  ds.allow_delete_layer_ = true;
  ds.SetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST,
                     "<LayerCreationOptionList>"
                     "  <Option name='OPT1' type='string'/>"
                     "</LayerCreationOptionList>");

  const char* lco[] = {"OPT1=VAL", nullptr};
  EXPECT_NE(GDALDatasetCreateLayer(hDS, "LayerOne", nullptr, wkbNone, lco),
            nullptr);
  // CreateLayer with non-linear geometry when ODsCCurveGeometries is false ->
  // linearizes!
  EXPECT_NE(GDALDatasetCreateLayer(hDS, "LayerTwo", nullptr, wkbCircularString,
                                   nullptr),
            nullptr);
  EXPECT_EQ(ds.GetLayer(1)->GetGeomType(), wkbLineString);

  OGRGeomFieldDefn gfd("geom", wkbPoint);
  EXPECT_NE(GDALDatasetCreateLayerFromGeomFieldDefn(
                hDS, "LayerThree", OGRGeomFieldDefn::ToHandle(&gfd), nullptr),
            nullptr);

  EXPECT_EQ(GDALDatasetGetLayerCount(hDS), 3);
  EXPECT_NE(GDALDatasetGetLayer(hDS, 0), nullptr);
  EXPECT_EQ(GDALDatasetIsLayerPrivate(hDS, 0), 0);

  // GetLayerByName & GetLayerIndex: exact match, case-insensitive match, not
  // found, null
  EXPECT_EQ(ds.GetLayerByName(nullptr), nullptr);
  EXPECT_NE(GDALDatasetGetLayerByName(hDS, "LayerOne"), nullptr);
  EXPECT_NE(GDALDatasetGetLayerByName(hDS, "layerone"), nullptr);
  EXPECT_EQ(GDALDatasetGetLayerByName(hDS, "NoSuchLayer"), nullptr);
  EXPECT_EQ(ds.GetLayerIndex("LayerTwo"), 1);
  EXPECT_EQ(ds.GetLayerIndex("layertwo"), 1);
  EXPECT_EQ(ds.GetLayerIndex("NoSuchLayer"), -1);

  // Layers and ConstLayers iterators & copy/move/post-increment/indexing
  auto layers = ds.GetLayers();
  EXPECT_EQ(layers.size(), 3U);
  EXPECT_NE(layers[0], nullptr);
  EXPECT_NE(layers[static_cast<size_t>(1)], nullptr);
  EXPECT_NE(layers["LayerThree"], nullptr);
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
    EXPECT_NE(l, nullptr);
    ++lyr_cnt;
  }
  EXPECT_EQ(lyr_cnt, 3);

  const ExposedDataset& const_ds = ds;
  auto const_layers = const_ds.GetLayers();
  EXPECT_EQ(const_layers.size(), 3U);
  EXPECT_NE(const_layers[0], nullptr);
  EXPECT_NE(const_layers[static_cast<size_t>(1)], nullptr);
  EXPECT_NE(const_layers["LayerThree"], nullptr);
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
    EXPECT_NE(l, nullptr);
    ++lyr_cnt;
  }
  EXPECT_EQ(lyr_cnt, 3);

  // CopyLayer tests:
  // 1) Layer with 1 geom field, fields, metadata, and DST_SRSWKT reprojection
  OGRSpatialReference srs_src;
  srs_src.SetWellKnownGeogCS("WGS84");
  srs_src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  CustomLayer copy_src("copy_src", wkbPoint, &srs_src);
  copy_src.SetMetadataItem("META_K", "META_V");
  OGRFieldDefn f_id("id", OFTInteger);
  OGRFieldDefn f_name("name", OFTString);
  EXPECT_EQ(copy_src.CreateField(&f_id, TRUE), OGRERR_NONE);
  EXPECT_EQ(copy_src.CreateField(&f_name, TRUE), OGRERR_NONE);
  auto feat = std::make_unique<OGRFeature>(copy_src.GetLayerDefn());
  feat->SetFID(10);
  feat->SetField("id", 42);
  feat->SetField("name", "pt1");
  OGRPoint pt(2.0, 49.0);
  feat->SetGeometry(&pt);
  EXPECT_EQ(copy_src.CreateFeature(feat.get()), OGRERR_NONE);

  // Pre-create 'id' field on destination layer, and test
  // null_defn_before_field_
  ds.precreated_fields_.push_back(f_id);
  ds.created_layers_null_defn_before_field_ = true;
  OGRSpatialReference srs_dst;
  srs_dst.importFromEPSG(3857);
  char* dst_wkt = nullptr;
  srs_dst.exportToWkt(&dst_wkt);
  std::string dst_srs_opt = std::string("DST_SRSWKT=") + dst_wkt;
  CPLFree(dst_wkt);
  const char* copy_opts[] = {dst_srs_opt.c_str(), "COPY_MD=YES", nullptr};

  OGRLayerH hCopied = GDALDatasetCopyLayer(hDS, OGRLayer::ToHandle(&copy_src),
                                           "CopiedOne", copy_opts);
  ASSERT_NE(hCopied, nullptr);
  EXPECT_STREQ(OGRLayer::FromHandle(hCopied)->GetMetadataItem("META_K"),
               "META_V");
  ds.precreated_fields_.clear();
  ds.created_layers_null_defn_before_field_ = false;

  // 2) CopyLayer with transactions enabled on destination layer and multiple
  // geom fields
  CustomLayer copy_src2("copy_src2", wkbNone);
  OGRGeomFieldDefn gfd1("geom1", wkbPoint);
  OGRGeomFieldDefn gfd2("geom2", wkbPoint);
  EXPECT_EQ(copy_src2.CreateGeomField(&gfd1, TRUE), OGRERR_NONE);
  EXPECT_EQ(copy_src2.CreateGeomField(&gfd2, TRUE), OGRERR_NONE);
  auto feat2 = std::make_unique<OGRFeature>(copy_src2.GetLayerDefn());
  feat2->SetFID(10);
  OGRPoint pt2(2.0, 49.0);
  feat2->SetGeomField(0, &pt2);
  feat2->SetGeomField(1, &pt2);
  EXPECT_EQ(copy_src2.CreateFeature(feat2.get()), OGRERR_NONE);

  ds.allow_create_geom_field_ = true;
  ds.created_layers_support_tx_ = true;
  ds.created_layers_skip_adding_field_ = true;  // Triggers buggy driver warning
  EXPECT_NE(ds.CopyLayer(&copy_src2, "CopiedTx", nullptr), nullptr);

  // 3) CopyLayer with transactions where CreateFeature fails ->
  // RollbackTransaction!
  ds.created_layers_create_feature_err_ = OGRERR_FAILURE;
  OGRLayer* copied_rollback =
      ds.CopyLayer(&copy_src2, "CopiedRollback", nullptr);
  ASSERT_NE(copied_rollback, nullptr);
  EXPECT_TRUE(static_cast<CustomLayer*>(copied_rollback)->rollback_called_);
  ds.created_layers_create_feature_err_ = OGRERR_NONE;
  ds.created_layers_support_tx_ = false;
  ds.created_layers_skip_adding_field_ = false;

  // DeleteLayer
  EXPECT_EQ(GDALDatasetDeleteLayer(hDS, 0), OGRERR_NONE);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, ExecuteSQLDDLAndSelectComprehensive) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  GDALDatasetReleaseResultSet(nullptr, nullptr);
  EXPECT_EQ(GDALDatasetExecuteSQL(nullptr, "SELECT 1", nullptr, nullptr),
            nullptr);
  EXPECT_EQ(GDALDatasetAbortSQL(nullptr), OGRERR_FAILURE);

  EXPECT_TRUE(GDALDataset::IsGenericSQLDialect("OGRSQL"));
  EXPECT_TRUE(GDALDataset::IsGenericSQLDialect("SQLITE"));
  EXPECT_FALSE(GDALDataset::IsGenericSQLDialect("NATIVE"));
  EXPECT_FALSE(GDALDataset::IsGenericSQLDialect(nullptr));

  ExposedDataset ds;
  ds.allow_delete_layer_ = true;
  ds.SetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS, "OGRSQL SQLITE CUSTOM");
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  EXPECT_EQ(GDALDatasetAbortSQL(hDS), OGRERR_UNSUPPORTED_OPERATION);

  auto lyr = std::make_unique<CustomLayer>("tbl", wkbPoint);
  CustomLayer* lyr_ptr = lyr.get();
  ds.layers_.push_back(std::move(lyr));

  // 1.ALTER TABLE ADD [COLUMN] covering all SQL types in
  // GDALDatasetParseSQLType
  const char* add_col_stmts[] = {
      "ALTER TABLE tbl ADD col_int INTEGER",
      "ALTER TABLE tbl ADD COLUMN col_int_arr INTEGER[]",
      "ALTER TABLE tbl ADD col_flt FLOAT",
      "ALTER TABLE tbl ADD col_num NUMERIC(10, 2)",
      "ALTER TABLE tbl ADD col_flt_arr FLOAT[]",
      "ALTER TABLE tbl ADD col_str CHARACTER(20)",
      "ALTER TABLE tbl ADD col_str_arr TEXT[]",
      "ALTER TABLE tbl ADD col_date DATE",
      "ALTER TABLE tbl ADD col_time TIME",
      "ALTER TABLE tbl ADD col_dt TIMESTAMP",
      "ALTER TABLE tbl ADD col_unk UNKNOWN_TYPE",
      "ALTER TABLE tbl ADD bad_syntax",
      "ALTER TABLE no_such_tbl ADD col_x INTEGER",
  };
  for (const char* stmt : add_col_stmts) {
    GDALDatasetExecuteSQL(hDS, stmt, nullptr, "UNSUPPORTED_DIALECT");
  }
  EXPECT_GE(lyr_ptr->GetLayerDefn()->GetFieldCount(), 10);

  // Add Int64 and Boolean subtypes so BuildParseInfo covers them
  OGRFieldDefn f_bool("col_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  EXPECT_EQ(lyr_ptr->CreateField(&f_bool, TRUE), OGRERR_NONE);
  OGRFieldDefn f_i64("col_i64", OFTInteger64);
  EXPECT_EQ(lyr_ptr->CreateField(&f_i64, TRUE), OGRERR_NONE);
  OGRFieldDefn f_i64_bool("col_i64_bool", OFTInteger64);
  f_i64_bool.SetSubType(OFSTBoolean);
  EXPECT_EQ(lyr_ptr->CreateField(&f_i64_bool, TRUE), OGRERR_NONE);
  lyr_ptr->SetMetadataItem(OLMD_FID64, "YES");
  lyr_ptr->fid_col_ = "ogc_fid";

  // 2. ALTER TABLE RENAME [COLUMN]
  GDALDatasetExecuteSQL(hDS,
                        "ALTER TABLE tbl RENAME COLUMN col_unk TO col_ren1",
                        nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl RENAME col_ren1 TO col_ren2",
                        nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl RENAME bad_syntax", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE no_tbl RENAME a TO b", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl RENAME no_col TO b", nullptr,
                        nullptr);

  // 3. ALTER TABLE ALTER [COLUMN]
  GDALDatasetExecuteSQL(
      hDS, "ALTER TABLE tbl ALTER COLUMN col_ren2 TYPE VARCHAR(30)", nullptr,
      nullptr);
  // Same type and width -> l_nFlags == 0
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl ALTER col_ren2 TYPE VARCHAR(30)",
                        nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl ALTER col_ren2 TYPE INTEGER",
                        nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl ALTER bad_syntax", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE no_tbl ALTER a TYPE INTEGER", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl ALTER no_col TYPE INTEGER",
                        nullptr, nullptr);

  // 4. ALTER TABLE DROP [COLUMN]
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl DROP COLUMN col_ren2", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl DROP col_dt", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl DROP", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE no_tbl DROP a", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl DROP no_col", nullptr, nullptr);

  // 5. ALTER TABLE RENAME TO and unsupported ALTER TABLE
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl RENAME TO tbl_renamed", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl_renamed RENAME TO tbl", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE no_tbl RENAME TO tbl2", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "ALTER TABLE tbl FOOBAR BAZ", nullptr, nullptr);

  // 6. CREATE INDEX and DROP INDEX
  GDALDatasetExecuteSQL(hDS, "CREATE INDEX bad syntax", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "CREATE INDEX ON no_tbl USING col_int", nullptr,
                        nullptr);
  // Layer without index support
  GDALDatasetExecuteSQL(hDS, "CREATE INDEX ON tbl USING col_int", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP INDEX bad syntax", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP INDEX ON no_tbl", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP INDEX ON tbl", nullptr, nullptr);

  // Enable index support on tbl
  lyr_ptr->EnableDummyIndex();
  GDALDatasetExecuteSQL(hDS, "CREATE INDEX ON tbl USING no_col", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "CREATE INDEX ON tbl USING col_int", nullptr,
                        nullptr);
  lyr_ptr->dummy_idx_->create_err_ = OGRERR_FAILURE;
  CPLErrorReset();
  GDALDatasetExecuteSQL(hDS, "CREATE INDEX ON tbl USING col_int", nullptr,
                        nullptr);
  lyr_ptr->dummy_idx_->create_err_ = OGRERR_NONE;

  GDALDatasetExecuteSQL(hDS, "DROP INDEX ON tbl USING no_col", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP INDEX ON tbl USING col_int", nullptr,
                        nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP INDEX ON tbl", nullptr, nullptr);
  lyr_ptr->dummy_idx_->drop_err_ = OGRERR_FAILURE;
  GDALDatasetExecuteSQL(hDS, "DROP INDEX ON tbl", nullptr, nullptr);
  lyr_ptr->dummy_idx_->drop_err_ = OGRERR_NONE;

  // 7. SELECT, UNION ALL, JOIN, and SQLite dialect
  auto feat = std::make_unique<OGRFeature>(lyr_ptr->GetLayerDefn());
  feat->SetFID(1);
  feat->SetField("col_int", 10);
  EXPECT_EQ(lyr_ptr->CreateFeature(feat.get()), OGRERR_NONE);

  // Preparse error and non-existent table
  EXPECT_EQ(GDALDatasetExecuteSQL(hDS, "SELECT FROM", nullptr, nullptr),
            nullptr);
  EXPECT_EQ(
      GDALDatasetExecuteSQL(hDS, "SELECT * FROM no_such_tbl", nullptr, nullptr),
      nullptr);

  // Valid SELECT with WHERE
  OGRLayerH hRes = GDALDatasetExecuteSQL(
      hDS, "SELECT *, ogc_fid FROM tbl WHERE col_int = 10", nullptr, nullptr);
  ASSERT_NE(hRes, nullptr);
  GDALDatasetReleaseResultSet(hDS, hRes);

  // UNION ALL (valid and failing second leg)
  OGRLayerH hUnion = GDALDatasetExecuteSQL(
      hDS, "SELECT col_int FROM tbl UNION ALL SELECT col_int FROM tbl", nullptr,
      nullptr);
  ASSERT_NE(hUnion, nullptr);
  GDALDatasetReleaseResultSet(hDS, hUnion);

  EXPECT_EQ(GDALDatasetExecuteSQL(
                hDS, "SELECT col_int FROM tbl UNION ALL SELECT * FROM no_tbl",
                nullptr, nullptr),
            nullptr);

  // Secondary datasource JOIN failure
  EXPECT_EQ(GDALDatasetExecuteSQL(
                hDS,
                "SELECT * FROM tbl LEFT JOIN '/vsimem/no_ds.json'.l2 "
                "ON tbl.col_int = l2.id",
                nullptr, nullptr),
            nullptr);

  // SQLite dialect
  OGRLayerH hSqlite =
      GDALDatasetExecuteSQL(hDS, "SELECT col_int FROM tbl", nullptr, "SQLite");
  if (hSqlite) {
    GDALDatasetReleaseResultSet(hDS, hSqlite);
  }

  // 8. DROP TABLE
  GDALDatasetExecuteSQL(hDS, "DROP TABLE bad syntax extra", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP TABLE no_such_tbl", nullptr, nullptr);
  GDALDatasetExecuteSQL(hDS, "DROP TABLE tbl", nullptr, nullptr);
  EXPECT_EQ(ds.GetLayerCount(), 0);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, StyleTableGetNextFeatureGetFeaturesAndTransactions) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALDatasetGetStyleTable(nullptr), nullptr);
  GDALDatasetSetStyleTableDirectly(nullptr, nullptr);
  GDALDatasetSetStyleTable(nullptr, nullptr);
  GDALDatasetResetReading(nullptr);
  EXPECT_EQ(
      GDALDatasetGetNextFeature(nullptr, nullptr, nullptr, nullptr, nullptr),
      nullptr);
  EXPECT_EQ(GDALDatasetTestCapability(nullptr, ODsCCreateLayer), 0);
  EXPECT_EQ(GDALDatasetStartTransaction(nullptr, FALSE), OGRERR_INVALID_HANDLE);
  EXPECT_EQ(GDALDatasetCommitTransaction(nullptr), OGRERR_INVALID_HANDLE);
  EXPECT_EQ(GDALDatasetRollbackTransaction(nullptr), OGRERR_INVALID_HANDLE);
  EXPECT_FALSE(GDALDatasetSetQueryLoggerFunc(nullptr, nullptr, nullptr));

  ExposedDataset ds;
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // StyleTable
  EXPECT_EQ(GDALDatasetGetStyleTable(hDS), nullptr);
  OGRStyleTable st;
  st.AddStyle("s1", "PEN(c:#FF0000)");
  GDALDatasetSetStyleTable(hDS, reinterpret_cast<OGRStyleTableH>(&st));
  EXPECT_NE(GDALDatasetGetStyleTable(hDS), nullptr);
  // Replace style table
  GDALDatasetSetStyleTable(hDS, reinterpret_cast<OGRStyleTableH>(&st));
  auto* st_direct = new OGRStyleTable();
  GDALDatasetSetStyleTableDirectly(hDS,
                                   reinterpret_cast<OGRStyleTableH>(st_direct));
  EXPECT_EQ(ds.GetStyleTable(), st_direct);

  // Transactions and QueryLogger default implementations
  EXPECT_EQ(GDALDatasetStartTransaction(hDS, FALSE),
            OGRERR_UNSUPPORTED_OPERATION);
  EXPECT_EQ(GDALDatasetCommitTransaction(hDS), OGRERR_UNSUPPORTED_OPERATION);
  EXPECT_EQ(GDALDatasetRollbackTransaction(hDS), OGRERR_UNSUPPORTED_OPERATION);
  EXPECT_FALSE(GDALDatasetSetQueryLoggerFunc(hDS, nullptr, nullptr));
  EXPECT_EQ(GDALDatasetTestCapability(hDS, nullptr), 0);

  // GetNextFeature and GetFeatures() across 2 layers
  auto l1 = std::make_unique<CustomLayer>("l1");
  auto f1 = std::make_unique<OGRFeature>(l1->GetLayerDefn());
  f1->SetFID(1);
  EXPECT_EQ(l1->CreateFeature(f1.get()), OGRERR_NONE);

  auto l2 = std::make_unique<CustomLayer>("l2");
  auto f2 = std::make_unique<OGRFeature>(l2->GetLayerDefn());
  f2->SetFID(2);
  EXPECT_EQ(l2->CreateFeature(f2.get()), OGRERR_NONE);

  CustomLayer* l1_ptr = l1.get();
  ds.layers_.push_back(std::move(l1));
  ds.layers_.push_back(std::move(l2));

  // Case 1: OLCFastFeatureCount is true on all layers
  GDALDatasetResetReading(hDS);
  OGRLayerH hBelong = nullptr;
  double pct = 0.0;
  OGRFeatureH hFeat1 = GDALDatasetGetNextFeature(hDS, &hBelong, &pct,
                                                 GDALDummyProgress, nullptr);
  ASSERT_NE(hFeat1, nullptr);
  EXPECT_DOUBLE_EQ(pct, 0.5);
  OGRFeature::DestroyFeature(OGRFeature::FromHandle(hFeat1));

  OGRFeatureH hFeat2 = GDALDatasetGetNextFeature(hDS, &hBelong, &pct,
                                                 GDALDummyProgress, nullptr);
  ASSERT_NE(hFeat2, nullptr);
  EXPECT_DOUBLE_EQ(pct, 1.0);
  OGRFeature::DestroyFeature(OGRFeature::FromHandle(hFeat2));

  // EOF reached
  EXPECT_EQ(GDALDatasetGetNextFeature(hDS, &hBelong, &pct, GDALDummyProgress,
                                      nullptr),
            nullptr);
  // Subsequent call after EOF (nCurrentLayerIdx < 0)
  EXPECT_EQ(GDALDatasetGetNextFeature(hDS, &hBelong, &pct, GDALDummyProgress,
                                      nullptr),
            nullptr);

  // Case 2: OLCFastFeatureCount is false on l1 -> nTotalFeatures < 0
  l1_ptr->fast_feature_count_ = false;
  GDALDatasetResetReading(hDS);
  for (int i = 0; i < 2; ++i) {
    OGRFeatureH hf = GDALDatasetGetNextFeature(hDS, &hBelong, &pct,
                                               GDALDummyProgress, nullptr);
    ASSERT_NE(hf, nullptr);
    OGRFeature::DestroyFeature(OGRFeature::FromHandle(hf));
  }

  // Case 3: GetFeatures() range-for loop
  int feat_count = 0;
  for (auto&& pair : ds.GetFeatures()) {
    EXPECT_NE(pair.feature, nullptr);
    EXPECT_NE(pair.layer, nullptr);
    ++feat_count;
  }
  EXPECT_EQ(feat_count, 2);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, ReadWriteMutexAndParentLockSharing) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  ExposedDataset parent;
  parent.eAccess = GA_Update;
  parent.RasterInitialize(4, 4);
  parent.SetBand(1, new CustomBand(&parent, 1));

  parent.InitRWLock();
  EXPECT_EQ(parent.EnterReadWrite(GF_Read), TRUE);
  parent.TemporarilyDropReadWriteLock();
  parent.ReacquireReadWriteLock();
  parent.LeaveReadWrite();

  EXPECT_TRUE(parent.AcquireMutex());
  parent.ReleaseMutex();

  // Child sharing lock with parent
  ExposedDataset child;
  child.ShareLockWithParentDataset(&parent);
  child.InitRWLock();
  EXPECT_EQ(child.EnterReadWrite(GF_Write), TRUE);
  child.TemporarilyDropReadWriteLock();
  child.ReacquireReadWriteLock();
  child.LeaveReadWrite();
  EXPECT_TRUE(child.AcquireMutex());
  child.ReleaseMutex();
  child.DisableReadWriteMutex();

  // Standalone dataset with GDAL_ENABLE_READ_WRITE_MUTEX=NO
  {
    CPLConfigOptionSetter no_mutex("GDAL_ENABLE_READ_WRITE_MUTEX", "NO", false);
    ExposedDataset ds_nomutex;
    ds_nomutex.eAccess = GA_Update;
    EXPECT_EQ(ds_nomutex.EnterReadWrite(GF_Write), FALSE);
  }

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, FieldDomainsRelationshipsCompressionAndClearStats) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  GDALDatasetClearStatistics(nullptr);
  EXPECT_EQ(GDALDatasetGetFieldDomainNames(nullptr, nullptr), nullptr);
  EXPECT_EQ(GDALDatasetGetFieldDomain(nullptr, "d"), nullptr);
  EXPECT_FALSE(GDALDatasetAddFieldDomain(nullptr, nullptr, nullptr));
  EXPECT_FALSE(GDALDatasetDeleteFieldDomain(nullptr, "d", nullptr));
  EXPECT_FALSE(GDALDatasetUpdateFieldDomain(nullptr, nullptr, nullptr));

  EXPECT_EQ(GDALDatasetGetRelationshipNames(nullptr, nullptr), nullptr);
  EXPECT_EQ(GDALDatasetGetRelationship(nullptr, "r"), nullptr);
  EXPECT_FALSE(GDALDatasetAddRelationship(nullptr, nullptr, nullptr));
  EXPECT_FALSE(GDALDatasetDeleteRelationship(nullptr, "r", nullptr));
  EXPECT_FALSE(GDALDatasetUpdateRelationship(nullptr, nullptr, nullptr));

  EXPECT_EQ(GDALDatasetGetCompressionFormats(nullptr, 0, 0, 1, 1, 1, nullptr),
            nullptr);
  EXPECT_EQ(GDALDatasetReadCompressedData(nullptr, "JPEG", 0, 0, 1, 1, 1,
                                          nullptr, nullptr, nullptr, nullptr),
            CE_Failure);

  ExposedDataset ds;
  ds.RasterInitialize(4, 4);
  ds.SetBand(1, new CustomBand(&ds, 1));
  ds.SetBand(2, new CustomBand(&ds, 2));
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // ClearStatistics without and with RootGroup
  EXPECT_EQ(ds.GetRootGroup(), nullptr);
  GDALDatasetClearStatistics(hDS);
  ds.root_group_ = std::make_shared<DummyGroup>();
  GDALDatasetClearStatistics(hDS);
  EXPECT_TRUE(ds.root_group_->cleared_);

  // RawBinaryLayout
  GDALDataset::RawBinaryLayout layout;
  EXPECT_FALSE(ds.GetRawBinaryLayout(layout));

  // SetEnableOverviews / AreOverviewsEnabled / IsAllBands
  EXPECT_TRUE(ds.AreOverviewsEnabled());
  ds.SetEnableOverviews(false);
  EXPECT_FALSE(ds.AreOverviewsEnabled());
  ds.SetEnableOverviews(true);

  int bands_ok[] = {1, 2};
  int bands_bad[] = {2, 1};
  EXPECT_TRUE(ds.IsAllBands(2, nullptr));
  EXPECT_TRUE(ds.IsAllBands(2, bands_ok));
  EXPECT_FALSE(ds.IsAllBands(1, nullptr));
  EXPECT_FALSE(ds.IsAllBands(2, bands_bad));

  // FieldDomains: populate m_oMapFieldDomains to test GetFieldDomainNames &
  // GetFieldDomain
  OGRField min_val;
  min_val.Integer = 0;
  OGRField max_val;
  max_val.Integer = 10;
  auto range_dom = std::make_unique<OGRRangeFieldDomain>(
      "dom1", "desc", OFTInteger, OFSTNone, min_val, true, max_val, true);
  OGRFieldDomainH hDom = OGRFieldDomain::ToHandle(range_dom.get());

  char* reason = nullptr;
  EXPECT_FALSE(GDALDatasetAddFieldDomain(hDS, hDom, &reason));
  EXPECT_NE(reason, nullptr);
  CPLFree(reason);

  EXPECT_FALSE(GDALDatasetUpdateFieldDomain(hDS, hDom, &reason));
  EXPECT_NE(reason, nullptr);
  CPLFree(reason);

  EXPECT_FALSE(GDALDatasetDeleteFieldDomain(hDS, "dom1", &reason));
  EXPECT_NE(reason, nullptr);
  CPLFree(reason);

  ds.m_oMapFieldDomains["dom1"] = std::move(range_dom);
  char** dom_names = GDALDatasetGetFieldDomainNames(hDS, nullptr);
  EXPECT_EQ(CSLCount(dom_names), 1);
  CSLDestroy(dom_names);
  EXPECT_NE(GDALDatasetGetFieldDomain(hDS, "dom1"), nullptr);
  EXPECT_EQ(GDALDatasetGetFieldDomain(hDS, "no_such_dom"), nullptr);

  // Relationships
  EXPECT_EQ(GDALDatasetGetRelationshipNames(hDS, nullptr), nullptr);
  EXPECT_EQ(GDALDatasetGetRelationship(hDS, "rel1"), nullptr);
  GDALRelationship rel("rel1", "left_t", "right_t",
                       GDALRelationshipCardinality::GRC_ONE_TO_MANY);
  GDALRelationshipH hRel = GDALRelationship::ToHandle(&rel);

  EXPECT_FALSE(GDALDatasetAddRelationship(hDS, hRel, &reason));
  EXPECT_NE(reason, nullptr);
  CPLFree(reason);

  EXPECT_FALSE(GDALDatasetUpdateRelationship(hDS, hRel, &reason));
  EXPECT_NE(reason, nullptr);
  CPLFree(reason);

  EXPECT_FALSE(GDALDatasetDeleteRelationship(hDS, "rel1", &reason));
  EXPECT_NE(reason, nullptr);
  CPLFree(reason);

  // CompressionFormats and ReadCompressedData
  EXPECT_EQ(GDALDatasetGetCompressionFormats(hDS, 0, 0, 4, 4, 2, nullptr),
            nullptr);
  EXPECT_EQ(GDALDatasetReadCompressedData(hDS, "JPEG", 0, 0, 4, 4, 2, nullptr,
                                          nullptr, nullptr, nullptr),
            CE_Failure);

  // CanBeCloned and Clone
  EXPECT_FALSE(ds.CanBeCloned(GDAL_OF_RASTER, true));
  ds.m_bCanBeReopened = true;
  EXPECT_TRUE(ds.CanBeCloned(GDAL_OF_RASTER, true));
  EXPECT_FALSE(ds.CanBeCloned(GDAL_OF_VECTOR, true));
  EXPECT_EQ(ds.Clone(GDAL_OF_RASTER, true), nullptr);

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, GeolocationToPixelLineGetExtentAndBuildFilename) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALDatasetGeolocationToPixelLine(nullptr, 0, 0, nullptr, nullptr,
                                              nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetExtent(nullptr, nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALGetExtentWGS84LongLat(nullptr, nullptr), CE_Failure);

  ExposedDataset ds;
  ds.RasterInitialize(10, 10);
  GDALDatasetH hDS = GDALDataset::ToHandle(&ds);

  // Without georeferencing, GeolocationToPixelLine and GetExtent fail
  double px = 0.0, line = 0.0;
  EXPECT_EQ(GDALDatasetGeolocationToPixelLine(hDS, 0, 0, nullptr, &px, &line,
                                              nullptr),
            CE_Failure);
  OGREnvelope env;
  EXPECT_EQ(GDALGetExtent(hDS, &env, nullptr), CE_Failure);

  // Set WGS84 SRS and GeoTransform on ds
  ds.use_custom_srs_ = true;
  ds.custom_srs_ = std::make_unique<OGRSpatialReference>();
  ds.custom_srs_->SetWellKnownGeogCS("WGS84");
  ds.custom_srs_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds.has_custom_gt_ = true;
  ds.custom_gt_ = GDALGeoTransform(10.0, 1.0, 0.0, 50.0, 0.0, -1.0);

  // GeolocationToPixelLine with null SRS, TRADITIONAL_GIS_ORDER,
  // AUTHORITY_COMPLIANT, and OAMS_CUSTOM
  EXPECT_EQ(GDALDatasetGeolocationToPixelLine(hDS, 15.0, 45.0, nullptr, &px,
                                              &line, nullptr),
            CE_None);
  EXPECT_NEAR(px, 5.0, 1e-5);
  EXPECT_NEAR(line, 5.0, 1e-5);

  OGRSpatialReference srs_trad;
  srs_trad.SetWellKnownGeogCS("WGS84");
  srs_trad.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(ds.GeolocationToPixelLine(15.0, 45.0, &srs_trad, &px, &line),
            CE_None);

  OGRSpatialReference srs_auth;
  srs_auth.importFromEPSG(4326);
  srs_auth.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
  EXPECT_EQ(ds.GeolocationToPixelLine(45.0, 15.0, &srs_auth, &px, &line),
            CE_None);

  OGRSpatialReference srs_custom;
  srs_custom.SetWellKnownGeogCS("WGS84");
  srs_custom.SetDataAxisToSRSAxisMapping({1, 2});
  EXPECT_EQ(ds.GeolocationToPixelLine(15.0, 45.0, &srs_custom, &px, &line),
            CE_None);

  // GetExtent and GetExtentWGS84LongLat on raster dataset
  EXPECT_EQ(GDALGetExtent(hDS, &env, nullptr), CE_None);
  EXPECT_DOUBLE_EQ(env.MinX, 10.0);
  EXPECT_DOUBLE_EQ(env.MaxX, 20.0);
  EXPECT_EQ(GDALGetExtentWGS84LongLat(hDS, &env), CE_None);

  // GetExtent and TestCapability(GDsCFastGetExtent) on vector dataset
  ExposedDataset vec_ds;
  EXPECT_EQ(vec_ds.TestCapability(GDsCFastGetExtent), TRUE);
  auto vl1 = std::make_unique<CustomLayer>("vl1", wkbPoint, &srs_trad);
  vl1->extent_.MinX = 1.0;
  vl1->extent_.MaxX = 5.0;
  vl1->extent_.MinY = 2.0;
  vl1->extent_.MaxY = 6.0;
  CustomLayer* vl1_ptr = vl1.get();
  vec_ds.layers_.push_back(std::move(vl1));
  EXPECT_EQ(vec_ds.TestCapability(GDsCFastGetExtent), TRUE);
  vl1_ptr->fast_get_extent_ = false;
  EXPECT_EQ(vec_ds.TestCapability(GDsCFastGetExtent), FALSE);
  EXPECT_EQ(vec_ds.GetExtent(&env, nullptr), CE_None);
  EXPECT_DOUBLE_EQ(env.MinX, 1.0);
  EXPECT_DOUBLE_EQ(env.MaxY, 6.0);

  // BuildFilename
  EXPECT_EQ(GDALDataset::BuildFilename("foo.tif", "/ref/dir", false),
            "foo.tif");
  EXPECT_EQ(GDALDataset::BuildFilename("../foo.tif", "/abc/def", true),
            "/abc/foo.tif");
  EXPECT_EQ(GDALDataset::BuildFilename("GTIFF_DIR:1:rel.tif", "/abc", true),
            "GTIFF_DIR:1:/abc/rel.tif");
  EXPECT_EQ(GDALDataset::BuildFilename("NITF_IM:0:rel.ntf", "/abc", true),
            "NITF_IM:0:/abc/rel.ntf");
  EXPECT_EQ(GDALDataset::BuildFilename("NITF_IM:0:z:/rel.ntf", "/abc", true),
            "NITF_IM:0:z:/rel.ntf");
  EXPECT_EQ(
      GDALDataset::BuildFilename("RASTERLITE:rel.db,table=t", "/abc", true),
      "RASTERLITE:/abc/rel.db,table=t");
  EXPECT_EQ(GDALDataset::BuildFilename("TILEDB:\"rel.tdb\":arr", "/abc", true),
            "TILEDB:\"/abc/rel.tdb\":arr");

  CPLPopErrorHandler();
}

TEST(GdalDatasetTest, AsMDArrayComprehensive) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Error cases: 0 bands, non-uniform data types, invalid DIM_ORDER
  ExposedDataset ds_empty;
  EXPECT_EQ(ds_empty.AsMDArray(), nullptr);

  ExposedDataset ds_mixed;
  ds_mixed.RasterInitialize(4, 4);
  ds_mixed.SetBand(1, new CustomBand(&ds_mixed, 1, GDT_Byte));
  ds_mixed.SetBand(2, new CustomBand(&ds_mixed, 2, GDT_UInt16));
  EXPECT_EQ(ds_mixed.AsMDArray(), nullptr);

  ExposedDataset ds_bad_opt;
  ds_bad_opt.RasterInitialize(4, 4);
  ds_bad_opt.SetBand(1, new CustomBand(&ds_bad_opt, 1, GDT_Byte));
  const char* bad_dim_opts[] = {"DIM_ORDER=INVALID", nullptr};
  EXPECT_EQ(ds_bad_opt.AsMDArray(bad_dim_opts), nullptr);

  // 2. Int64 and UInt64 nodata datasets
  {
    auto* ds_i64 = new ExposedDataset();
    ds_i64->RasterInitialize(4, 4);
    auto* b = new CustomBand(ds_i64, 1, GDT_Int64);
    b->has_nodata_ = true;
    b->nodata_val_ = -999;
    ds_i64->SetBand(1, b);
    // SRS with North/East (EPSG:4326 authority compliant -> mapping {1,2} or
    // {2,1})
    ds_i64->use_custom_srs_ = true;
    ds_i64->custom_srs_ = std::make_unique<OGRSpatialReference>();
    ds_i64->custom_srs_->importFromEPSG(4326);
    ds_i64->custom_srs_->SetDataAxisToSRSAxisMapping({2, 1});
    const char* idx_opts[] = {"BAND_INDEXING_VAR_ITEM={Index}", nullptr};
    auto md = ds_i64->AsMDArray(idx_opts);
    ASSERT_NE(md, nullptr);
    EXPECT_NE(md->GetRawNoDataValue(), nullptr);
    EXPECT_NE(md->GetSpatialRef(), nullptr);
    md.reset();
    GDALClose(ds_i64);
  }

  {
    auto* ds_u64 = new ExposedDataset();
    ds_u64->RasterInitialize(4, 4);
    auto* b = new CustomBand(ds_u64, 1, GDT_UInt64);
    b->has_nodata_ = true;
    b->nodata_val_ = 999;
    ds_u64->SetBand(1, b);
    // SRS with East/North (projected EPSG:32631 -> mapping {1,2})
    ds_u64->use_custom_srs_ = true;
    ds_u64->custom_srs_ = std::make_unique<OGRSpatialReference>();
    ds_u64->custom_srs_->importFromEPSG(32631);
    ds_u64->custom_srs_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    const char* ci_opts[] = {"BAND_INDEXING_VAR_ITEM={ColorInterpretation}",
                             "DIM_ORDER=Y,X,Band", nullptr};
    auto md = ds_u64->AsMDArray(ci_opts);
    ASSERT_NE(md, nullptr);
    EXPECT_NE(md->GetRawNoDataValue(), nullptr);
    EXPECT_NE(md->GetSpatialRef(), nullptr);
    EXPECT_EQ(md->GetBlockSize().size(), 3U);
    md.reset();
    GDALClose(ds_u64);
  }

  // 3. Multi-band Byte dataset testing all BAND_INDEXING_VAR_ITEM / TYPE
  // options,
  //    GeoTransform indexing vars, Offset/Scale/Unit/Attributes, Read & Write
  auto* ds_mb = new ExposedDataset();
  ds_mb->eAccess = GA_Update;
  ds_mb->SetDescription("/vsimem/md_test.bin");
  ds_mb->SetMetadataItem("ATTR1", "VAL1");
  ds_mb->RasterInitialize(4, 4);
  ds_mb->has_custom_gt_ = true;
  ds_mb->custom_gt_ = GDALGeoTransform(0.0, 1.0, 0.0, 10.0, 0.0, -1.0);
  ds_mb->use_custom_srs_ = true;
  ds_mb->custom_srs_ = std::make_unique<OGRSpatialReference>();
  ds_mb->custom_srs_->SetWellKnownGeogCS("WGS84");

  auto* mb1 = new CustomBand(ds_mb, 1, GDT_Byte, 2, 2);
  mb1->unit_ = "m";
  mb1->has_nodata_ = true;
  mb1->nodata_val_ = 255.0;
  mb1->has_offset_ = true;
  mb1->offset_val_ = 1.5;
  mb1->has_scale_ = true;
  mb1->scale_val_ = 2.5;
  mb1->SetMetadataItem("WAVELENGTH", "550");

  auto* mb2 = new CustomBand(ds_mb, 2, GDT_Byte, 2, 2);
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

  // Default ({Description})
  {
    auto md = ds_mb->AsMDArray(nullptr);
    ASSERT_NE(md, nullptr);
    EXPECT_TRUE(md->IsWritable());
    EXPECT_EQ(md->GetFilename(), "/vsimem/md_test.bin");
    EXPECT_EQ(md->GetDimensions().size(), 3U);
    EXPECT_EQ(md->GetDataType().GetNumericDataType(), GDT_Byte);
    EXPECT_EQ(md->GetUnit(), "m");
    EXPECT_NE(md->GetRawNoDataValue(), nullptr);
    bool has_val = false;
    GDALDataType st_dt = GDT_Byte;
    EXPECT_DOUBLE_EQ(md->GetOffset(&has_val, &st_dt), 1.5);
    EXPECT_TRUE(has_val);
    EXPECT_DOUBLE_EQ(md->GetScale(&has_val, &st_dt), 2.5);
    EXPECT_TRUE(has_val);
    EXPECT_NE(md->GetSpatialRef(), nullptr);
    EXPECT_EQ(md->GetBlockSize().size(), 3U);
    EXPECT_EQ(md->GetAttributes(nullptr).size(), 1U);

    // Read with positive steps
    GUInt64 start[3] = {0, 0, 0};
    size_t count[3] = {2, 2, 2};
    GInt64 step[3] = {1, 1, 1};
    GPtrDiff_t stride[3] = {4, 2, 1};
    GByte buf[8] = {0};
    EXPECT_TRUE(md->Read(start, count, step, stride, md->GetDataType(), buf));
    EXPECT_TRUE(md->Write(start, count, step, stride, md->GetDataType(), buf));

    // Read with negative steps in Y and X
    GUInt64 start_neg[3] = {0, 1, 1};
    GInt64 step_neg[3] = {1, -1, -1};
    EXPECT_TRUE(
        md->Read(start_neg, count, step_neg, stride, md->GetDataType(), buf));
  }

  // Custom metadata item with String, Integer, Real, and {None}
  const char* types[] = {"String", "Integer", "Real"};
  for (const char* t : types) {
    const char* opts[] = {"BAND_INDEXING_VAR_ITEM=WAVELENGTH",
                          CPLSPrintf("BAND_INDEXING_VAR_TYPE=%s", t), nullptr};
    EXPECT_NE(ds_mb->AsMDArray(opts), nullptr);
  }
  const char* none_opts[] = {"BAND_INDEXING_VAR_ITEM={None}", nullptr};
  EXPECT_NE(ds_mb->AsMDArray(none_opts), nullptr);

  GDALClose(ds_mb);
  CPLPopErrorHandler();
}

}  // namespace
