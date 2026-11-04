// Copyright 2017 Google Inc. All Rights Reserved.
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

// Tests for OGRLayer.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrlayer.cpp
//   http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayer.cpp

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

constexpr char kLayerName[] = "basic layer";

class BasicLayer : public OGRLayer {
 private:
  OGRFeatureDefn* feature_defn_ = nullptr;

 public:
  BasicLayer() {
    feature_defn_ = new OGRFeatureDefn(kLayerName);
    feature_defn_->Reference();
  }
  ~BasicLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }
  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char*) const override { return FALSE; }
};

class ScanTestLayer : public OGRLayer {
 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
  std::vector<std::unique_ptr<OGRFeature>> features_;
  size_t cur_index_ = 0;

 public:
  explicit ScanTestLayer(const char* name,
                         OGRwkbGeometryType geom_type = wkbUnknown,
                         OGRSpatialReference* srs = nullptr) {
    feature_defn_ = new OGRFeatureDefn(name);
    feature_defn_->Reference();
    feature_defn_->SetGeomType(geom_type);
    if (feature_defn_->GetGeomFieldCount() > 0 && srs) {
      feature_defn_->GetGeomFieldDefn(0)->SetSpatialRef(srs);
    }
  }

  ~ScanTestLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }

  void AddFeature(std::unique_ptr<OGRFeature> feat) {
    features_.push_back(std::move(feat));
  }

  void ResetReading() override { cur_index_ = 0; }

  OGRFeature* GetNextFeature() override {
    while (cur_index_ < features_.size()) {
      OGRFeature* feat = features_[cur_index_++].get();
      if ((m_poFilterGeom == nullptr ||
           FilterGeometry(feat->GetGeometryRef())) &&
          (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(feat))) {
        return feat->Clone();
      }
    }
    return nullptr;
  }

  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char*) const override { return FALSE; }

  using OGRLayer::FieldChangeType;
  using OGRLayer::FilterGeometry;
  using OGRLayer::FilterWKBGeometry;
  using OGRLayer::FinishRollbackTransaction;
  using OGRLayer::InstallFilter;
  using OGRLayer::m_apoFieldDefnChanges;
  using OGRLayer::m_apoGeomFieldDefnChanges;
  using OGRLayer::m_bFilterIsEnvelope;
  using OGRLayer::m_poAttrQuery;
  using OGRLayer::m_poFilterGeom;
  using OGRLayer::m_pPreparedFilterGeom;
  using OGRLayer::m_sFilterEnvelope;
  using OGRLayer::PrepareStartTransaction;
  using OGRLayer::ValidateGeometryFieldIndexForSetSpatialFilter;

  OGRLayer::GetSupportedSRSListRetType supported_srs_;
  void AddSupportedSRS(std::unique_ptr<OGRSpatialReference> srs) {
    supported_srs_.emplace_back(srs.release());
  }
  const OGRLayer::GetSupportedSRSListRetType& GetSupportedSRSList(
      int) override {
    return supported_srs_;
  }
};

class DelegatingUpdatableLayer : public OGRLayer {
 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
  std::map<GIntBig, std::unique_ptr<OGRFeature>> features_;
  GIntBig next_fid_ = 1;
  std::map<GIntBig, std::unique_ptr<OGRFeature>>::iterator iter_;

 public:
  explicit DelegatingUpdatableLayer(const char* name) {
    feature_defn_ = new OGRFeatureDefn(name);
    feature_defn_->Reference();
    iter_ = features_.begin();
  }
  ~DelegatingUpdatableLayer() override {
    if (feature_defn_) feature_defn_->Release();
  }

  void ResetReading() override { iter_ = features_.begin(); }
  OGRFeature* GetNextFeature() override {
    if (iter_ != features_.end()) {
      auto* f = iter_->second->Clone();
      ++iter_;
      return f;
    }
    return nullptr;
  }
  OGRFeature* GetFeature(GIntBig nFID) override {
    auto it = features_.find(nFID);
    if (it != features_.end()) {
      return it->second->Clone();
    }
    return nullptr;
  }
  OGRErr ICreateFeature(OGRFeature* poFeature) override {
    if (poFeature->GetFID() == OGRNullFID) {
      poFeature->SetFID(next_fid_++);
    }
    features_[poFeature->GetFID()] =
        std::unique_ptr<OGRFeature>(poFeature->Clone());
    return OGRERR_NONE;
  }
  OGRErr ISetFeature(OGRFeature* poFeature) override {
    features_[poFeature->GetFID()] =
        std::unique_ptr<OGRFeature>(poFeature->Clone());
    return OGRERR_NONE;
  }
  OGRErr DeleteFeature(GIntBig nFID) override {
    features_.erase(nFID);
    return OGRERR_NONE;
  }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char* pszCap) const override {
    return EQUAL(pszCap, OLCRandomWrite) ? TRUE : FALSE;
  }
};

class UnfilteredTestLayer : public OGRLayer {
 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
  std::vector<std::unique_ptr<OGRFeature>> features_;
  size_t cur_index_ = 0;

 public:
  bool fail_create_field_ = false;
  bool fail_create_feature_ = false;

  explicit UnfilteredTestLayer(const char* name,
                               OGRwkbGeometryType geom_type = wkbUnknown) {
    feature_defn_ = new OGRFeatureDefn(name);
    feature_defn_->Reference();
    feature_defn_->SetGeomType(geom_type);
  }

  ~UnfilteredTestLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }

  void AddFeature(std::unique_ptr<OGRFeature> feat) {
    features_.push_back(std::move(feat));
  }

  void ResetReading() override { cur_index_ = 0; }

  OGRFeature* GetNextFeature() override {
    if (cur_index_ < features_.size()) {
      return features_[cur_index_++]->Clone();
    }
    return nullptr;
  }

  GIntBig GetFeatureCount(int) override {
    return static_cast<GIntBig>(features_.size());
  }

  OGRErr CreateField(const OGRFieldDefn* poField, int) override {
    if (fail_create_field_) return OGRERR_FAILURE;
    feature_defn_->AddFieldDefn(poField);
    return OGRERR_NONE;
  }

  OGRErr ICreateFeature(OGRFeature* poFeature) override {
    if (fail_create_feature_) return OGRERR_FAILURE;
    features_.push_back(std::unique_ptr<OGRFeature>(poFeature->Clone()));
    return OGRERR_NONE;
  }

  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char*) const override { return FALSE; }
};

// ---------------------------------------------------------------------------
// Baseline Tests Preserved from 2017
// ---------------------------------------------------------------------------

TEST(LayerTest, FeatureCount) {
  auto layer = std::make_unique<BasicLayer>();

  EXPECT_EQ(-1, layer->GetFeatureCount(FALSE));
  EXPECT_EQ(0, layer->GetFeatureCount(TRUE));
  EXPECT_EQ(-1, OGR_L_GetFeatureCount(layer.get(), FALSE));
  EXPECT_EQ(0, OGR_L_GetFeatureCount(layer.get(), TRUE));
}

TEST(LayerTest, GetExtent) {
  auto layer = std::make_unique<BasicLayer>();
  OGREnvelope envelope;
  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(&envelope, FALSE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);

  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(&envelope, TRUE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);

  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(0, &envelope, FALSE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);

  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(0, &envelope, TRUE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);

  // Geometry field does not exist.
  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(1, &envelope, FALSE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);

  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(1, &envelope, TRUE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);
}

TEST(LayerTest, GetExtentC) {
  auto layer = std::make_unique<BasicLayer>();
  OGREnvelope envelope;
  EXPECT_EQ(OGRERR_FAILURE, OGR_L_GetExtent(layer.get(), &envelope, FALSE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);

  EXPECT_EQ(OGRERR_FAILURE,
            OGR_L_GetExtentEx(layer.get(), 0, &envelope, FALSE));
  EXPECT_DOUBLE_EQ(0.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MinY);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxX);
  EXPECT_DOUBLE_EQ(0.0, envelope.MaxY);
}

TEST(LayerTest, ReferenceCounting) {
  auto layer = std::make_unique<BasicLayer>();
  EXPECT_EQ(0, layer->GetRefCount());
  EXPECT_EQ(1, layer->Reference());
  EXPECT_EQ(1, layer->GetRefCount());
  EXPECT_EQ(0, layer->Dereference());
  EXPECT_EQ(-1, layer->Dereference());
}

TEST(LayerTest, ReferenceCountingC) {
  auto layer = std::make_unique<BasicLayer>();
  EXPECT_EQ(0, OGR_L_GetRefCount(layer.get()));
  EXPECT_EQ(1, OGR_L_Reference(layer.get()));
  EXPECT_EQ(0, OGR_L_Dereference(layer.get()));
  EXPECT_EQ(-1, OGR_L_Dereference(layer.get()));
}

// ---------------------------------------------------------------------------
// Extended Tests for OGRLayer Base Implementations & Fallbacks
// ---------------------------------------------------------------------------

TEST(LayerTest, GetExtent3DAndC) {
  auto layer = std::make_unique<BasicLayer>();
  OGREnvelope3D env3d;
  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent3D(0, &env3d, false));
  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent3D(0, &env3d, true));
  EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent3D(1, &env3d, true));

  EXPECT_EQ(OGRERR_FAILURE, OGR_L_GetExtent3D(layer.get(), 0, &env3d, false));
  EXPECT_EQ(OGRERR_FAILURE, OGR_L_GetExtent3D(layer.get(), 0, &env3d, true));
}

TEST(LayerTest, BaseUnsupportedOperationsAndC) {
  auto layer = std::make_unique<BasicLayer>();
  WithQuietHandler quiet_handler;

  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());
  OGRFeature feat(defn);
  feat.SetFID(1);

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->SetFeature(&feat));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, OGR_L_SetFeature(layer.get(), &feat));

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->CreateFeature(&feat));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_CreateFeature(layer.get(), &feat));

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->DeleteFeature(1));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, OGR_L_DeleteFeature(layer.get(), 1));

  OGRFieldDefn field_defn("f1", OFTInteger);
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->CreateField(&field_defn));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_CreateField(layer.get(), &field_defn, true));

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->DeleteField(0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, OGR_L_DeleteField(layer.get(), 0));

  int map[1] = {0};
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->ReorderFields(map));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_ReorderFields(layer.get(), map));
  EXPECT_EQ(OGRERR_FAILURE, layer->ReorderField(0, 0));
  EXPECT_EQ(OGRERR_FAILURE, OGR_L_ReorderField(layer.get(), 0, 0));

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            layer->AlterFieldDefn(0, &field_defn, 0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_AlterFieldDefn(layer.get(), 0, &field_defn, 0));

  OGRGeomFieldDefn geom_defn("g1", wkbPoint);
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->CreateGeomField(&geom_defn));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_CreateGeomField(
                layer.get(), OGRGeomFieldDefn::ToHandle(&geom_defn), true));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            layer->AlterGeomFieldDefn(0, &geom_defn, 0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_AlterGeomFieldDefn(
                layer.get(), 0, OGRGeomFieldDefn::ToHandle(&geom_defn), 0));

  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());
  EXPECT_EQ(OGRERR_NONE, OGR_L_SyncToDisk(layer.get()));

  EXPECT_EQ(OGRERR_NONE, layer->StartTransaction());
  EXPECT_EQ(OGRERR_NONE, OGR_L_StartTransaction(layer.get()));

  EXPECT_EQ(OGRERR_NONE, layer->CommitTransaction());
  EXPECT_EQ(OGRERR_NONE, OGR_L_CommitTransaction(layer.get()));

  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->RollbackTransaction());
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_RollbackTransaction(layer.get()));

  const char* ignored[] = {"f1", nullptr};
  EXPECT_EQ(OGRERR_FAILURE, layer->SetIgnoredFields(ignored));
  EXPECT_EQ(OGRERR_FAILURE, OGR_L_SetIgnoredFields(layer.get(), ignored));
  EXPECT_EQ(OGRERR_NONE, layer->SetIgnoredFields(nullptr));
  EXPECT_EQ(OGRERR_NONE, OGR_L_SetIgnoredFields(layer.get(), nullptr));

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->Rename("new_name"));
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
              OGR_L_Rename(layer.get(), "new_name"));

    EXPECT_EQ(OGRERR_FAILURE, layer->SetActiveSRS(0, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, OGR_L_SetActiveSRS(layer.get(), 0, nullptr));
  }

  EXPECT_TRUE(layer->GetSupportedSRSList(0).empty());
  int srs_count = -1;
  EXPECT_EQ(nullptr, OGR_L_GetSupportedSRSList(layer.get(), 0, &srs_count));
  EXPECT_EQ(0, srs_count);

  EXPECT_EQ(nullptr, layer->GetDataset());
  EXPECT_EQ(nullptr, OGR_L_GetDataset(layer.get()));

  EXPECT_STREQ(kLayerName, layer->GetName());
  EXPECT_STREQ(kLayerName, OGR_L_GetName(layer.get()));

  EXPECT_STREQ("", layer->GetFIDColumn());
  EXPECT_STREQ("", OGR_L_GetFIDColumn(layer.get()));

  EXPECT_STREQ("", layer->GetGeometryColumn());
  EXPECT_STREQ("", OGR_L_GetGeometryColumn(layer.get()));

  EXPECT_EQ(nullptr, layer->GetSpatialRef());
  EXPECT_EQ(nullptr, OGR_L_GetSpatialRef(layer.get()));

  EXPECT_EQ(0, layer->GetFeaturesRead());
  EXPECT_EQ(0, OGR_L_GetFeaturesRead(layer.get()));

  EXPECT_EQ(OGRFeatureDefn::ToHandle(defn), OGR_L_GetLayerDefn(layer.get()));
  EXPECT_EQ(-1, layer->FindFieldIndex("nonexistent", true));
  EXPECT_EQ(-1, OGR_L_FindFieldIndex(layer.get(), "nonexistent", true));

  EXPECT_FALSE(layer->TestCapability("RandomCap"));
  EXPECT_FALSE(OGR_L_TestCapability(layer.get(), "RandomCap"));
}

TEST(LayerTest, ScanFallbacksAndFilters) {
  auto srs = std::make_unique<OGRSpatialReference>();
  srs->importFromEPSG(4326);
  srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto layer =
      std::make_unique<ScanTestLayer>("scan_layer", wkbPolygon, srs.get());
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  OGRFieldDefn f_int("f_int", OFTInteger);
  defn->AddFieldDefn(&f_int);

  for (int i = 0; i < 5; ++i) {
    auto f = std::make_unique<OGRFeature>(defn);
    f->SetFID(100 + i);
    f->SetField(0, i * 10);

    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(i, i, i * 2.0);
    ring->addPoint(i, i + 1, i * 2.0 + 1.0);
    ring->addPoint(i + 1, i + 1, i * 2.0 + 1.0);
    ring->addPoint(i + 1, i, i * 2.0);
    ring->addPoint(i, i, i * 2.0);
    poly->addRingDirectly(ring.release());
    f->SetGeometryDirectly(poly.release());

    layer->AddFeature(std::move(f));
  }

  // Feature with no geometry
  {
    auto f_nogeom = std::make_unique<OGRFeature>(defn);
    f_nogeom->SetFID(200);
    f_nogeom->SetField(0, 999);
    layer->AddFeature(std::move(f_nogeom));
  }

  // Feature with empty geometry
  {
    auto f_empty = std::make_unique<OGRFeature>(defn);
    f_empty->SetFID(201);
    f_empty->SetField(0, 888);
    f_empty->SetGeometryDirectly(new OGRPolygon());
    layer->AddFeature(std::move(f_empty));
  }

  // Sequential scan GetFeatureCount
  EXPECT_EQ(7, layer->GetFeatureCount(true));
  EXPECT_EQ(7, OGR_L_GetFeatureCount(layer.get(), true));

  // Sequential scan GetExtent (2D)
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env, true));
  EXPECT_DOUBLE_EQ(0.0, env.MinX);
  EXPECT_DOUBLE_EQ(0.0, env.MinY);
  EXPECT_DOUBLE_EQ(5.0, env.MaxX);
  EXPECT_DOUBLE_EQ(5.0, env.MaxY);

  // Sequential scan GetExtent3D
  OGREnvelope3D env3d;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent3D(0, &env3d, true));
  EXPECT_DOUBLE_EQ(0.0, env3d.MinX);
  EXPECT_DOUBLE_EQ(0.0, env3d.MinZ);
  EXPECT_DOUBLE_EQ(9.0, env3d.MaxZ);

  // Fallback GetFeature(fid)
  std::unique_ptr<OGRFeature> feat102(layer->GetFeature(102));
  ASSERT_NE(nullptr, feat102);
  EXPECT_EQ(102, feat102->GetFID());
  EXPECT_EQ(20, feat102->GetFieldAsInteger(0));

  std::unique_ptr<OGRFeature> feat_c(
      static_cast<OGRFeature*>(OGR_L_GetFeature(layer.get(), 103)));
  ASSERT_NE(nullptr, feat_c);
  EXPECT_EQ(103, feat_c->GetFID());

  EXPECT_EQ(nullptr, layer->GetFeature(9999));

  // Fallback SetNextByIndex
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, layer->SetNextByIndex(-1));
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, OGR_L_SetNextByIndex(layer.get(), -1));
  EXPECT_EQ(OGRERR_NONE, layer->SetNextByIndex(2));
  std::unique_ptr<OGRFeature> f_idx2(layer->GetNextFeature());
  ASSERT_NE(nullptr, f_idx2);
  EXPECT_EQ(102, f_idx2->GetFID());

  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, layer->SetNextByIndex(100));

  // Attribute filter
  EXPECT_EQ(OGRERR_NONE,
            layer->SetAttributeFilter("f_int >= 20 AND f_int <= 40"));
  EXPECT_EQ(OGRERR_NONE, OGR_L_SetAttributeFilter(
                             layer.get(), "f_int >= 20 AND f_int <= 40"));
  EXPECT_EQ(3, layer->GetFeatureCount(true));

  // Attribute filter needing geometry
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("OGR_GEOMETRY IS NOT NULL"));
  EXPECT_TRUE(layer->AttributeFilterEvaluationNeedsGeometry());
  EXPECT_EQ(6, layer->GetFeatureCount(true));

  // Invalid attribute filter
  {
    WithQuietHandler quiet_handler;
    EXPECT_NE(OGRERR_NONE, layer->SetAttributeFilter("INVALID SQL SYNTAX !!!"));
  }
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter(nullptr));

  // Spatial filter rect & filter geometry
  EXPECT_EQ(OGRERR_NONE, layer->SetSpatialFilterRect(1.5, 1.5, 3.5, 3.5));
  OGR_L_SetSpatialFilterRectEx(layer.get(), 0, 1.5, 1.5, 3.5, 3.5);
  EXPECT_NE(nullptr, layer->GetSpatialFilter());
  EXPECT_NE(nullptr, OGR_L_GetSpatialFilter(layer.get()));
  EXPECT_GT(layer->GetFeatureCount(true), 0);

  // WKB Geometry filtering
  OGRPoint pt(2.0, 2.0);
  int wkb_size = pt.WkbSize();
  std::vector<GByte> wkb(wkb_size);
  EXPECT_EQ(OGRERR_NONE, pt.exportToWkb(wkb.data()));
  OGREnvelope env_pt;
  EXPECT_TRUE(layer->FilterWKBGeometry(wkb.data(), wkb.size(), false, env_pt));

  OGRPoint pt_out(100.0, 100.0);
  std::vector<GByte> wkb_out(pt_out.WkbSize());
  EXPECT_EQ(OGRERR_NONE, pt_out.exportToWkb(wkb_out.data()));
  OGREnvelope env_out;
  EXPECT_FALSE(
      layer->FilterWKBGeometry(wkb_out.data(), wkb_out.size(), false, env_out));

  // Reset spatial filter
  EXPECT_EQ(OGRERR_NONE, layer->SetSpatialFilter(nullptr));
  OGR_L_SetSpatialFilter(layer.get(), nullptr);
  EXPECT_EQ(nullptr, layer->GetSpatialFilter());

  OSRCleanup();
}

TEST(LayerTest, GeometryConversionsAndPrecision) {
  CPLConfigOptionSetter prec_opt("OGR_APPLY_GEOM_SET_PRECISION", "TRUE", false);
  auto layer = std::make_unique<DelegatingUpdatableLayer>("conv_layer");
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());
  defn->SetGeomType(wkbLineString);

  // Precision rounding
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.1;
  prec.dfZResolution = 0.5;
  prec.dfMResolution = 1.0;
  defn->GetGeomFieldDefn(0)->SetCoordinatePrecision(prec);

  OGRFeature feat(defn);
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(1.234, 5.678, 10.123);
  ls->addPoint(9.876, 4.321, 20.456);
  feat.SetGeometryDirectly(ls.release());

  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  const OGRLineString* res_ls = feat.GetGeometryRef()->toLineString();
  ASSERT_NE(nullptr, res_ls);
  EXPECT_NEAR(1.2, res_ls->getX(0), 1e-3);
  EXPECT_NEAR(5.7, res_ls->getY(0), 1e-3);
  EXPECT_NEAR(9.9, res_ls->getX(1), 1e-3);
  EXPECT_NEAR(4.3, res_ls->getY(1), 1e-3);

  // Test curve linearization and M stripping in ConvertGeomsIfNecessary
  {
    auto circ = std::make_unique<OGRCircularString>();
    circ->addPoint(0.0, 0.0, 0.0, 1.0);
    circ->addPoint(1.0, 1.0, 0.0, 2.0);
    circ->addPoint(2.0, 0.0, 0.0, 3.0);
    OGRFeature feat_circ(defn);
    feat_circ.SetGeometryDirectly(circ.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat_circ));
    ASSERT_NE(nullptr, feat_circ.GetGeometryRef());
    EXPECT_EQ(wkbLineString25D, feat_circ.GetGeometryRef()->getGeometryType());
    EXPECT_FALSE(OGR_GT_HasM(feat_circ.GetGeometryRef()->getGeometryType()));
  }
}

TEST(LayerTest, DelegatingUpdatableLayerOperations) {
  auto layer = std::make_unique<DelegatingUpdatableLayer>("upd_layer");
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  OGRFieldDefn f_name("name", OFTString);
  defn->AddFieldDefn(&f_name);

  // 1. Upsert feature on base OGRLayer returns OGRERR_UNSUPPORTED_OPERATION
  OGRFeature f1(defn);
  f1.SetFID(OGRNullFID);
  f1.SetField(0, "Alice");
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->UpsertFeature(&f1));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_UpsertFeature(layer.get(), &f1));

  // Create features using ICreateFeature
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&f1));
  EXPECT_EQ(1, f1.GetFID());

  OGRFeature f2(defn);
  f2.SetFID(OGRNullFID);
  f2.SetField(0, "Bob");
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&f2));
  EXPECT_EQ(2, f2.GetFID());

  // 2. UpdateFeature on existing feature
  int updated_field_idx = 0;
  OGRFeature f_upd(defn);
  f_upd.SetFID(2);
  f_upd.SetField(0, "BobUpdated");
  f_upd.SetStyleString("PEN(c:#FF0000)");
  EXPECT_EQ(OGRERR_NONE, layer->UpdateFeature(&f_upd, 1, &updated_field_idx, 0,
                                              nullptr, true));

  std::unique_ptr<OGRFeature> fetched2(layer->GetFeature(2));
  ASSERT_NE(nullptr, fetched2);
  EXPECT_STREQ("BobUpdated", fetched2->GetFieldAsString(0));
  EXPECT_STREQ("PEN(c:#FF0000)", fetched2->GetStyleString());

  // UpdateFeature via C API
  EXPECT_EQ(OGRERR_NONE,
            OGR_L_UpdateFeature(layer.get(), &f_upd, 1, &updated_field_idx, 0,
                                nullptr, true));

  // UpdateFeature non-existing feature
  OGRFeature f_nonexist(defn);
  f_nonexist.SetFID(999);
  f_nonexist.SetField(0, "Ghost");
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE,
            layer->UpdateFeature(&f_nonexist, 1, &updated_field_idx, 0, nullptr,
                                 false));

  // Error paths: invalid field indices
  int bad_idx = -1;
  EXPECT_EQ(OGRERR_FAILURE,
            layer->UpdateFeature(&f_upd, 1, &bad_idx, 0, nullptr, false));
  bad_idx = 100;
  EXPECT_EQ(OGRERR_FAILURE,
            layer->UpdateFeature(&f_upd, 1, &bad_idx, 0, nullptr, false));
  EXPECT_EQ(OGRERR_FAILURE,
            layer->UpdateFeature(&f_upd, 0, nullptr, 1, &bad_idx, false));

  // Unsupported operation on layer without OLCRandomWrite
  BasicLayer basic_layer;
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            basic_layer.UpdateFeature(&f_upd, 0, nullptr, 0, nullptr, false));
}

TEST(LayerTest, TransactionsRollbackSchema) {
  auto layer = std::make_unique<ScanTestLayer>("trans_layer");
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  OGRFieldDefn f1("f1", OFTInteger);
  defn->AddFieldDefn(&f1);

  // Prepare start transaction saves schema
  layer->PrepareStartTransaction();

  // Add a field during transaction
  OGRFieldDefn f2("f2", OFTString);
  defn->AddFieldDefn(&f2);
  EXPECT_EQ(2, defn->GetFieldCount());
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>(f2), 1,
      ScanTestLayer::FieldChangeType::ADD_FIELD, "");

  // Rollback restores schema
  layer->FinishRollbackTransaction("");
  EXPECT_EQ(1, defn->GetFieldCount());
  EXPECT_STREQ("f1", defn->GetFieldDefn(0)->GetNameRef());
}

TEST(LayerTest, StyleTableOperationsAndC) {
  auto layer = std::make_unique<BasicLayer>();

  EXPECT_EQ(nullptr, layer->GetStyleTable());
  EXPECT_EQ(nullptr, OGR_L_GetStyleTable(layer.get()));

  auto style_table = std::make_unique<OGRStyleTable>();

  layer->SetStyleTable(style_table.get());
  EXPECT_NE(nullptr, layer->GetStyleTable());

  OGR_L_SetStyleTable(layer.get(), style_table.get());
  EXPECT_NE(nullptr, OGR_L_GetStyleTable(layer.get()));

  layer->SetStyleTableDirectly(style_table.release());
  EXPECT_NE(nullptr, layer->GetStyleTable());

  auto style_table2 = std::make_unique<OGRStyleTable>();
  OGR_L_SetStyleTableDirectly(layer.get(), style_table2.release());
  EXPECT_NE(nullptr, OGR_L_GetStyleTable(layer.get()));

  layer->SetStyleTableDirectly(nullptr);
  EXPECT_EQ(nullptr, layer->GetStyleTable());
}

TEST(LayerTest, FeatureIteratorAndGeometryTypes) {
  auto layer = std::make_unique<ScanTestLayer>("iter_layer", wkbPolygon);
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  for (int i = 0; i < 3; ++i) {
    auto f = std::make_unique<OGRFeature>(defn);
    f->SetFID(i);
    auto poly = std::make_unique<OGRPolygon>();
    f->SetGeometryDirectly(poly.release());
    layer->AddFeature(std::move(f));
  }

  int count = 0;
  for (auto&& feat : *layer) {
    EXPECT_NE(nullptr, feat);
    count++;
  }
  EXPECT_EQ(3, count);

  // GetGeometryTypes
  int entry_count = 0;
  OGRGeometryTypeCounter* counters =
      layer->GetGeometryTypes(0, 0, entry_count, nullptr, nullptr);
  ASSERT_NE(nullptr, counters);
  EXPECT_EQ(1, entry_count);
  EXPECT_EQ(wkbPolygon, counters[0].eGeomType);
  EXPECT_EQ(3, counters[0].nCount);
  CPLFree(counters);

  int entry_count_c = 0;
  OGRGeometryTypeCounter* counters_c = OGR_L_GetGeometryTypes(
      layer.get(), 0, 0, &entry_count_c, nullptr, nullptr);
  ASSERT_NE(nullptr, counters_c);
  EXPECT_EQ(1, entry_count_c);
  EXPECT_EQ(wkbPolygon, counters_c[0].eGeomType);
  EXPECT_EQ(3, counters_c[0].nCount);
  CPLFree(counters_c);
}

// ---------------------------------------------------------------------------
// All 7 Overlay Operations and C API
// ---------------------------------------------------------------------------

TEST(LayerTest, AllSevenOverlayOperationsAndC) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto ds = std::make_unique<MEMDataset>();

  // Layer 1: Input polygon [0, 0] to [10, 10]
  OGRLayer* lyr1 = ds->CreateLayer("lyr1", &srs, wkbPolygon);
  ASSERT_NE(nullptr, lyr1);
  OGRFieldDefn f1("id1", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&f1));

  {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    feat->SetField(0, 10);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(0, 10);
    ring->addPoint(10, 10);
    ring->addPoint(10, 0);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(feat.get()));
  }

  // Layer 2: Method polygon [5, 0] to [15, 10]
  OGRLayer* lyr2 = ds->CreateLayer("lyr2", &srs, wkbPolygon);
  ASSERT_NE(nullptr, lyr2);
  OGRFieldDefn f2("id2", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr2->CreateField(&f2));

  {
    auto feat = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
    feat->SetField(0, 20);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(5, 0);
    ring->addPoint(5, 10);
    ring->addPoint(15, 10);
    ring->addPoint(15, 0);
    ring->addPoint(5, 0);
    poly->addRingDirectly(ring.release());
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, lyr2->CreateFeature(feat.get()));
  }

  CPLStringList options;
  options.AddString("INPUT_PREFIX=in_");
  options.AddString("METHOD_PREFIX=m_");
  options.AddString("PROMOTE_TO_MULTI=YES");
  options.AddString("USE_PREPARED_GEOMETRIES=YES");
  options.AddString("PRETEST_CONTAINMENT=YES");
  options.AddString("KEEP_LOWER_DIMENSION_GEOMETRIES=NO");

  // 1. Intersection
  {
    OGRLayer* res = ds->CreateLayer("res_inter", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->Intersection(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(1, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_inter_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE, OGR_L_Intersection(lyr1, lyr2, res_c, options.List(),
                                              nullptr, nullptr));
    EXPECT_EQ(1, res_c->GetFeatureCount(true));
  }

  // 2. Union
  {
    OGRLayer* res = ds->CreateLayer("res_union", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->Union(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(3, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_union_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              OGR_L_Union(lyr1, lyr2, res_c, options.List(), nullptr, nullptr));
    EXPECT_EQ(3, res_c->GetFeatureCount(true));
  }

  // 3. SymDifference
  {
    OGRLayer* res = ds->CreateLayer("res_symdiff", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->SymDifference(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(2, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_symdiff_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              OGR_L_SymDifference(lyr1, lyr2, res_c, options.List(), nullptr,
                                  nullptr));
    EXPECT_EQ(2, res_c->GetFeatureCount(true));
  }

  // 4. Identity
  {
    OGRLayer* res = ds->CreateLayer("res_identity", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->Identity(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(2, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_identity_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE, OGR_L_Identity(lyr1, lyr2, res_c, options.List(),
                                          nullptr, nullptr));
    EXPECT_EQ(2, res_c->GetFeatureCount(true));
  }

  // 5. Update
  {
    OGRLayer* res = ds->CreateLayer("res_update", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->Update(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(2, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_update_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE, OGR_L_Update(lyr1, lyr2, res_c, options.List(),
                                        nullptr, nullptr));
    EXPECT_EQ(2, res_c->GetFeatureCount(true));
  }

  // 6. Clip
  {
    OGRLayer* res = ds->CreateLayer("res_clip", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->Clip(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(1, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_clip_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              OGR_L_Clip(lyr1, lyr2, res_c, options.List(), nullptr, nullptr));
    EXPECT_EQ(1, res_c->GetFeatureCount(true));
  }

  // 7. Erase
  {
    OGRLayer* res = ds->CreateLayer("res_erase", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              lyr1->Erase(lyr2, res, options.List(), nullptr, nullptr));
    EXPECT_EQ(1, res->GetFeatureCount(true));

    OGRLayer* res_c = ds->CreateLayer("res_erase_c", &srs, wkbPolygon);
    EXPECT_EQ(OGRERR_NONE,
              OGR_L_Erase(lyr1, lyr2, res_c, options.List(), nullptr, nullptr));
    EXPECT_EQ(1, res_c->GetFeatureCount(true));
  }

  // Progress cancellation test
  {
    OGRLayer* res_cancel = ds->CreateLayer("res_cancel", &srs, wkbPolygon);
    auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
    EXPECT_EQ(OGRERR_FAILURE, lyr1->Intersection(lyr2, res_cancel, nullptr,
                                                 cancel_cb, nullptr));
  }

  OSRCleanup();
}

TEST(LayerTest, TransactionsRollbackSchemaExtended) {
  auto layer = std::make_unique<ScanTestLayer>("rollback_ext");
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  OGRFieldDefn f0("f0", OFTInteger);
  defn->AddFieldDefn(&f0);
  OGRFieldDefn f1("f1", OFTString);
  defn->AddFieldDefn(&f1);

  // 1. DELETE_FIELD rollback with iField = 0 (when multiple fields exist so
  // reorder loop runs)
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>("f_deleted", OFTReal), 0,
      ScanTestLayer::FieldChangeType::DELETE_FIELD, "sp1");
  layer->FinishRollbackTransaction("sp1");
  EXPECT_EQ(3, defn->GetFieldCount());
  EXPECT_STREQ("f_deleted", defn->GetFieldDefn(0)->GetNameRef());

  // 2. ALTER_FIELD rollback (both valid and invalid iField)
  OGRFieldDefn f_altered("f_altered", OFTInteger);
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>(f_altered), 0,
      ScanTestLayer::FieldChangeType::ALTER_FIELD, "");
  layer->FinishRollbackTransaction("");
  EXPECT_STREQ("f_altered", defn->GetFieldDefn(0)->GetNameRef());

  // ALTER_FIELD with invalid field index
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>("f_bad", OFTInteger), 100,
      ScanTestLayer::FieldChangeType::ALTER_FIELD, "");
  layer->FinishRollbackTransaction("");
  EXPECT_EQ(3, defn->GetFieldCount());

  // Negative field index
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>("f_neg", OFTInteger), -1,
      ScanTestLayer::FieldChangeType::ALTER_FIELD, "");
  layer->FinishRollbackTransaction("");
  EXPECT_EQ(3, defn->GetFieldCount());

  // Savepoint matching and non-matching
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>("f_sp", OFTInteger), 0,
      ScanTestLayer::FieldChangeType::ALTER_FIELD, "target_sp");
  layer->m_apoFieldDefnChanges.emplace_back(
      std::make_unique<OGRFieldDefn>("f_sp2", OFTInteger), 0,
      ScanTestLayer::FieldChangeType::ALTER_FIELD, "other_sp");
  layer->FinishRollbackTransaction("target_sp");
  EXPECT_EQ(3, defn->GetFieldCount());

  // 3. Geom field rollback: ADD_FIELD, ALTER_FIELD, DELETE_FIELD, savepoint
  OGRGeomFieldDefn gf("gf", wkbPoint);
  defn->AddGeomFieldDefn(&gf);
  layer->m_apoGeomFieldDefnChanges.emplace_back(
      std::make_unique<OGRGeomFieldDefn>(gf), 0,
      ScanTestLayer::FieldChangeType::ADD_FIELD, "sp_geom");
  layer->FinishRollbackTransaction("sp_geom");
  EXPECT_EQ(1, defn->GetGeomFieldCount());

  layer->m_apoGeomFieldDefnChanges.emplace_back(
      std::make_unique<OGRGeomFieldDefn>(gf), 0,
      ScanTestLayer::FieldChangeType::ALTER_FIELD, "");
  layer->FinishRollbackTransaction("");
  EXPECT_EQ(0, defn->GetGeomFieldCount());
}

TEST(LayerTest, ReorderFieldOperations) {
  auto layer = std::make_unique<ScanTestLayer>("reorder_layer");
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  for (int i = 0; i < 5; ++i) {
    OGRFieldDefn f(CPLSPrintf("f%d", i), OFTInteger);
    defn->AddFieldDefn(&f);
  }

  // Base implementation returns OGRERR_UNSUPPORTED_OPERATION from ReorderFields
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->ReorderField(1, 3));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->ReorderField(3, 1));
  EXPECT_EQ(OGRERR_NONE, layer->ReorderField(2, 2));
  EXPECT_EQ(OGRERR_FAILURE, layer->ReorderField(-1, 2));
  EXPECT_EQ(OGRERR_FAILURE, layer->ReorderField(2, 10));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_ReorderField(layer.get(), 1, 3));
}

TEST(LayerTest, FilterWKBGeometryAndGeometryFilters) {
  auto layer = std::make_unique<ScanTestLayer>("filter_layer", wkbPolygon);
  OGREnvelope env;

  // 1. FilterWKBGeometry with null filter
  auto pt = std::make_unique<OGRPoint>(5.0, 5.0);
  int wkb_size = pt->WkbSize();
  std::vector<GByte> wkb(wkb_size);
  pt->exportToWkb(wkb.data());
  EXPECT_TRUE(layer->FilterWKBGeometry(wkb.data(), wkb.size(), false, env));

  // 2. Spatial filter rectangle
  layer->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);
  EXPECT_TRUE(layer->FilterWKBGeometry(wkb.data(), wkb.size(), false, env));

  auto pt_out = std::make_unique<OGRPoint>(20.0, 20.0);
  std::vector<GByte> wkb_out(pt_out->WkbSize());
  pt_out->exportToWkb(wkb_out.data());
  EXPECT_FALSE(
      layer->FilterWKBGeometry(wkb_out.data(), wkb_out.size(), false, env));

  // Partial overlap: multipoint with one inside (5,5) and one outside (15,15)
  OGRMultiPoint mp_partial;
  mp_partial.addGeometry(pt.get());
  mp_partial.addGeometry(pt_out.get());
  EXPECT_TRUE(layer->FilterGeometry(&mp_partial));

  // MultiPoint with envelope intersecting [0,10] but no points inside: (-5, 5)
  // and (15, 5)
  OGRMultiPoint mp_no_point_in;
  OGRPoint p_left(-5.0, 5.0);
  OGRPoint p_right(15.0, 5.0);
  mp_no_point_in.addGeometry(&p_left);
  mp_no_point_in.addGeometry(&p_right);
  EXPECT_FALSE(layer->FilterGeometry(&mp_no_point_in));

  // LineString crossing from (-5, 5) to (15, 5): envelope intersects, no vertex
  // inside
  OGRLineString ls_cross;
  ls_cross.addPoint(-5.0, 5.0);
  ls_cross.addPoint(15.0, 5.0);
  EXPECT_TRUE(layer->FilterGeometry(&ls_cross));
  std::vector<GByte> wkb_cross(ls_cross.WkbSize());
  ls_cross.exportToWkb(wkb_cross.data());
  EXPECT_TRUE(
      layer->FilterWKBGeometry(wkb_cross.data(), wkb_cross.size(), false, env));

  // LineString outside envelope entirely
  OGRLineString ls_outside;
  ls_outside.addPoint(20.0, 5.0);
  ls_outside.addPoint(30.0, 5.0);
  EXPECT_FALSE(layer->FilterGeometry(&ls_outside));

  // 3. Spatial filter non-envelope triangle geometry (prepared geom path,
  // m_bFilterIsEnvelope == false)
  auto filter_poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(0, 10);
  ring->addPoint(10, 0);
  ring->addPoint(0, 0);
  filter_poly->addRingDirectly(ring.release());
  layer->SetSpatialFilter(filter_poly.get());

  OGRPoint pt_in_tri(2.0, 2.0);
  EXPECT_TRUE(layer->FilterGeometry(&pt_in_tri));
  std::vector<GByte> wkb_in_tri(pt_in_tri.WkbSize());
  pt_in_tri.exportToWkb(wkb_in_tri.data());
  EXPECT_TRUE(layer->FilterWKBGeometry(wkb_in_tri.data(), wkb_in_tri.size(),
                                       false, env));

  OGRPoint pt_in_bbox_out_tri(8.0, 8.0);
  EXPECT_FALSE(layer->FilterGeometry(&pt_in_bbox_out_tri));
  std::vector<GByte> wkb_bbox_out(pt_in_bbox_out_tri.WkbSize());
  pt_in_bbox_out_tri.exportToWkb(wkb_bbox_out.data());
  EXPECT_FALSE(layer->FilterWKBGeometry(wkb_bbox_out.data(),
                                        wkb_bbox_out.size(), false, env));

  // 4. Other geometry types
  OGRLineString ls_in;
  ls_in.addPoint(2.0, 2.0);
  ls_in.addPoint(3.0, 3.0);
  EXPECT_TRUE(layer->FilterGeometry(&ls_in));

  OGRMultiLineString mls;
  mls.addGeometry(&ls_in);
  EXPECT_TRUE(layer->FilterGeometry(&mls));

  OGRMultiPolygon mpoly;
  auto sub_poly = std::make_unique<OGRPolygon>();
  auto sub_ring = std::make_unique<OGRLinearRing>();
  sub_ring->addPoint(1, 1);
  sub_ring->addPoint(1, 2);
  sub_ring->addPoint(2, 2);
  sub_ring->addPoint(2, 1);
  sub_ring->addPoint(1, 1);
  sub_poly->addRingDirectly(sub_ring.release());
  mpoly.addGeometry(sub_poly.get());
  EXPECT_TRUE(layer->FilterGeometry(&mpoly));

  OGRGeometryCollection gc;
  gc.addGeometry(&pt_in_tri);
  EXPECT_TRUE(layer->FilterGeometry(&gc));
}

TEST(LayerTest, GetGeometryTypesExtended) {
  auto layer = std::make_unique<ScanTestLayer>("geom_types_ext", wkbUnknown);
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  OGRFieldDefn f("f", OFTInteger);
  defn->AddFieldDefn(&f);
  defn->GetFieldDefn(0)->SetIgnored(TRUE);

  OGRFieldDefn f2("f2", OFTString);
  defn->AddFieldDefn(&f2);

  OGRGeomFieldDefn gf("gf", wkbPoint);
  defn->AddGeomFieldDefn(&gf);

  OGRGeomFieldDefn gf2("gf2", wkbPolygon);
  defn->AddGeomFieldDefn(&gf2);
  defn->GetGeomFieldDefn(1)->SetIgnored(TRUE);

  defn->SetStyleIgnored(TRUE);

  // Invalid geom field
  int count = 0;
  EXPECT_EQ(nullptr, layer->GetGeometryTypes(-1, 0, count, nullptr, nullptr));
  EXPECT_EQ(nullptr, layer->GetGeometryTypes(10, 0, count, nullptr, nullptr));

  // Feature with nullptr geometry
  auto f_null = std::make_unique<OGRFeature>(defn);
  layer->AddFeature(std::move(f_null));

  // Feature with GeometryCollection25D containing TINZ
  auto f_tin = std::make_unique<OGRFeature>(defn);
  auto gc = std::make_unique<OGRGeometryCollection>();
  auto tin = std::make_unique<OGRTriangulatedSurface>();
  auto tri = std::make_unique<OGRTriangle>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0, 1);
  ring->addPoint(1, 0, 1);
  ring->addPoint(0, 1, 1);
  ring->addPoint(0, 0, 1);
  tri->addRingDirectly(ring.release());
  tin->addGeometryDirectly(tri.release());
  gc->addGeometryDirectly(tin.release());
  gc->set3D(TRUE);
  f_tin->SetGeometryDirectly(gc.release());
  layer->AddFeature(std::move(f_tin));

  // Features with Point and Polygon
  auto f_pt = std::make_unique<OGRFeature>(defn);
  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  f_pt->SetGeometryDirectly(pt.release());
  layer->AddFeature(std::move(f_pt));

  auto f_poly = std::make_unique<OGRFeature>(defn);
  auto poly = std::make_unique<OGRPolygon>();
  f_poly->SetGeometryDirectly(poly.release());
  layer->AddFeature(std::move(f_poly));

  // Run with flags
  OGRGeometryTypeCounter* counters = layer->GetGeometryTypes(
      0, OGR_GGT_GEOMCOLLECTIONZ_TINZ | OGR_GGT_STOP_IF_MIXED, count,
      GDALDummyProgress, nullptr);
  ASSERT_NE(nullptr, counters);
  CPLFree(counters);

  // Progress callback returning FALSE
  auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
  counters = layer->GetGeometryTypes(0, 0, count, cancel_cb, nullptr);
  EXPECT_EQ(nullptr, counters);
  EXPECT_EQ(0, count);
}

TEST(LayerTest, OverlayEdgeCasesAndErrorPaths) {
  auto in_lyr = std::make_unique<UnfilteredTestLayer>("in_lyr");
  OGRFieldDefn f_in("shared_name", OFTString);
  in_lyr->CreateField(&f_in, true);
  OGRFieldDefn f_in2("in_only", OFTInteger);
  in_lyr->CreateField(&f_in2, true);

  auto method_lyr = std::make_unique<UnfilteredTestLayer>("method_lyr");
  OGRFieldDefn f_m("shared_name", OFTString);
  method_lyr->CreateField(&f_m, true);
  OGRFieldDefn f_m2("m_only", OFTInteger);
  method_lyr->CreateField(&f_m2, true);

  // Features for in_lyr:
  // 1. null geom
  auto f1 = std::make_unique<OGRFeature>(in_lyr->GetLayerDefn());
  in_lyr->AddFeature(std::move(f1));

  // 2. Large polygon [0,0]-[10,10]
  auto f2 = std::make_unique<OGRFeature>(in_lyr->GetLayerDefn());
  auto poly2 = std::make_unique<OGRPolygon>();
  auto r2 = std::make_unique<OGRLinearRing>();
  r2->addPoint(0, 0);
  r2->addPoint(0, 10);
  r2->addPoint(10, 10);
  r2->addPoint(10, 0);
  r2->addPoint(0, 0);
  poly2->addRingDirectly(r2.release());
  f2->SetGeometryDirectly(poly2.release());
  in_lyr->AddFeature(std::move(f2));

  // 3. Small polygon [6,2]-[9,8]
  auto f3 = std::make_unique<OGRFeature>(in_lyr->GetLayerDefn());
  auto poly3 = std::make_unique<OGRPolygon>();
  auto r3 = std::make_unique<OGRLinearRing>();
  r3->addPoint(6, 2);
  r3->addPoint(6, 8);
  r3->addPoint(9, 8);
  r3->addPoint(9, 2);
  r3->addPoint(6, 2);
  poly3->addRingDirectly(r3.release());
  f3->SetGeometryDirectly(poly3.release());
  in_lyr->AddFeature(std::move(f3));

  // 4. Point (5, 5)
  auto f4 = std::make_unique<OGRFeature>(in_lyr->GetLayerDefn());
  f4->SetGeometryDirectly(new OGRPoint(5, 5));
  in_lyr->AddFeature(std::move(f4));

  // 5. LineString
  auto f5 = std::make_unique<OGRFeature>(in_lyr->GetLayerDefn());
  auto ls5 = std::make_unique<OGRLineString>();
  ls5->addPoint(0, 5);
  ls5->addPoint(10, 5);
  f5->SetGeometryDirectly(ls5.release());
  in_lyr->AddFeature(std::move(f5));

  // Features for method_lyr:
  // 1. null geom
  auto fm1 = std::make_unique<OGRFeature>(method_lyr->GetLayerDefn());
  method_lyr->AddFeature(std::move(fm1));

  // 2. Overlapping polygon [5,0]-[15,10]
  auto fm2 = std::make_unique<OGRFeature>(method_lyr->GetLayerDefn());
  auto polym2 = std::make_unique<OGRPolygon>();
  auto rm2 = std::make_unique<OGRLinearRing>();
  rm2->addPoint(5, 0);
  rm2->addPoint(5, 10);
  rm2->addPoint(15, 10);
  rm2->addPoint(15, 0);
  rm2->addPoint(5, 0);
  polym2->addRingDirectly(rm2.release());
  fm2->SetGeometryDirectly(polym2.release());
  method_lyr->AddFeature(std::move(fm2));

  // 3. Contained polygon [2,2]-[4,4]
  auto fm3 = std::make_unique<OGRFeature>(method_lyr->GetLayerDefn());
  auto polym3 = std::make_unique<OGRPolygon>();
  auto rm3 = std::make_unique<OGRLinearRing>();
  rm3->addPoint(2, 2);
  rm3->addPoint(2, 4);
  rm3->addPoint(4, 4);
  rm3->addPoint(4, 2);
  rm3->addPoint(2, 2);
  polym3->addRingDirectly(rm3.release());
  fm3->SetGeometryDirectly(polym3.release());
  method_lyr->AddFeature(std::move(fm3));

  // 4. Disjoint polygon [100,100]-[110,110]
  auto fm4 = std::make_unique<OGRFeature>(method_lyr->GetLayerDefn());
  auto polym4 = std::make_unique<OGRPolygon>();
  auto rm4 = std::make_unique<OGRLinearRing>();
  rm4->addPoint(100, 100);
  rm4->addPoint(100, 110);
  rm4->addPoint(110, 110);
  rm4->addPoint(110, 100);
  rm4->addPoint(100, 100);
  polym4->addRingDirectly(rm4.release());
  fm4->SetGeometryDirectly(polym4.release());
  method_lyr->AddFeature(std::move(fm4));

  auto progress_cb = [](double, const char*, void*) -> int { return TRUE; };

  CPLStringList opt;
  opt.AddString("PRETEST_CONTAINMENT=YES");
  opt.AddString("PROMOTE_TO_MULTI=YES");
  opt.AddString("USE_PREPARED_GEOMETRIES=YES");

  // Run all 7 overlay operations with this rich set of features
  {
    UnfilteredTestLayer res("res_union");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Union(method_lyr.get(), &res, opt.List(),
                                         progress_cb, nullptr));
  }
  {
    UnfilteredTestLayer res("res_inter");
    EXPECT_EQ(OGRERR_NONE,
              in_lyr->Intersection(method_lyr.get(), &res, opt.List(),
                                   progress_cb, nullptr));
  }
  {
    UnfilteredTestLayer res("res_symdiff");
    EXPECT_EQ(OGRERR_NONE,
              in_lyr->SymDifference(method_lyr.get(), &res, opt.List(),
                                    progress_cb, nullptr));
  }
  {
    UnfilteredTestLayer res("res_ident");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Identity(method_lyr.get(), &res, opt.List(),
                                            progress_cb, nullptr));
  }
  {
    UnfilteredTestLayer res("res_upd");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Update(method_lyr.get(), &res, opt.List(),
                                          progress_cb, nullptr));
  }
  {
    UnfilteredTestLayer res("res_clip");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Clip(method_lyr.get(), &res, opt.List(),
                                        progress_cb, nullptr));
  }
  {
    UnfilteredTestLayer res("res_erase");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Erase(method_lyr.get(), &res, opt.List(),
                                         progress_cb, nullptr));
  }

  // Set existing spatial filter on in_lyr and method_lyr to exercise
  // set_filter_from and filter deletion
  auto sf = std::make_unique<OGRPolygon>();
  auto sf_r = std::make_unique<OGRLinearRing>();
  sf_r->addPoint(-10, -10);
  sf_r->addPoint(-10, 200);
  sf_r->addPoint(200, 200);
  sf_r->addPoint(200, -10);
  sf_r->addPoint(-10, -10);
  sf->addRingDirectly(sf_r.release());
  in_lyr->SetSpatialFilter(sf.get());
  method_lyr->SetSpatialFilter(sf.get());

  // Run without INPUT_PREFIX and METHOD_PREFIX with
  // KEEP_LOWER_DIMENSION_GEOMETRIES=YES on wkbPolygon
  {
    UnfilteredTestLayer res("res_noprefix", wkbPolygon);
    CPLStringList opt_noprefix;
    opt_noprefix.AddString("KEEP_LOWER_DIMENSION_GEOMETRIES=YES");
    EXPECT_EQ(OGRERR_NONE,
              in_lyr->Union(method_lyr.get(), &res, opt_noprefix.List(),
                            progress_cb, nullptr));
    EXPECT_NE(nullptr,
              res.GetLayerDefn()->GetFieldDefn(
                  res.GetLayerDefn()->GetFieldIndex("input_shared_name")));
    EXPECT_NE(nullptr,
              res.GetLayerDefn()->GetFieldDefn(
                  res.GetLayerDefn()->GetFieldIndex("method_shared_name")));
  }

  // Failure in CreateField on result layer (both SKIP_FAILURES=NO and YES)
  {
    UnfilteredTestLayer res("res_fail_cfld");
    res.fail_create_field_ = true;
    CPLStringList opt_noskip;
    opt_noskip.AddString("SKIP_FAILURES=NO");
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Union(method_lyr.get(), &res, opt_noskip.List(), nullptr,
                            nullptr));

    CPLStringList opt_skip;
    opt_skip.AddString("SKIP_FAILURES=YES");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Union(method_lyr.get(), &res,
                                         opt_skip.List(), nullptr, nullptr));
  }

  // Cancel in first loop across all 7 operations
  auto cancel_in_loop1 = [](double, const char*, void*) -> int {
    return FALSE;
  };
  {
    UnfilteredTestLayer res("res_cancel1");
    EXPECT_EQ(OGRERR_FAILURE, in_lyr->Union(method_lyr.get(), &res, opt.List(),
                                            cancel_in_loop1, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Intersection(method_lyr.get(), &res, opt.List(),
                                   cancel_in_loop1, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->SymDifference(method_lyr.get(), &res, opt.List(),
                                    cancel_in_loop1, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Identity(method_lyr.get(), &res, opt.List(),
                               cancel_in_loop1, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, in_lyr->Update(method_lyr.get(), &res, opt.List(),
                                             cancel_in_loop1, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, in_lyr->Clip(method_lyr.get(), &res, opt.List(),
                                           cancel_in_loop1, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, in_lyr->Erase(method_lyr.get(), &res, opt.List(),
                                            cancel_in_loop1, nullptr));
  }

  // Cancel in second loop: progress returns FALSE on call 5
  auto cancel_in_loop2 = [](double, const char*, void* pArg) -> int {
    int* c = static_cast<int*>(pArg);
    return (++(*c) <= 4) ? TRUE : FALSE;
  };
  {
    int p_count = 0;
    UnfilteredTestLayer res("res_cancel2");
    EXPECT_EQ(OGRERR_FAILURE, in_lyr->Union(method_lyr.get(), &res, opt.List(),
                                            cancel_in_loop2, &p_count));
  }

  // Cancel in second loop when p >= 1.0
  auto cancel_in_loop2_end = [](double p, const char*, void*) -> int {
    return (p >= 1.0) ? FALSE : TRUE;
  };
  {
    UnfilteredTestLayer res("res_cancel2_end");
    EXPECT_EQ(OGRERR_FAILURE, in_lyr->Union(method_lyr.get(), &res, opt.List(),
                                            cancel_in_loop2_end, nullptr));
  }

  // Failure in CreateFeature on result layer (both SKIP_FAILURES=NO and YES)
  {
    UnfilteredTestLayer res("res_fail_cf");
    res.fail_create_feature_ = true;
    CPLStringList opt_noskip;
    opt_noskip.AddString("SKIP_FAILURES=NO");
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Union(method_lyr.get(), &res, opt_noskip.List(), nullptr,
                            nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Intersection(method_lyr.get(), &res, opt_noskip.List(),
                                   nullptr, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->SymDifference(method_lyr.get(), &res, opt_noskip.List(),
                                    nullptr, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Identity(method_lyr.get(), &res, opt_noskip.List(),
                               nullptr, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Update(method_lyr.get(), &res, opt_noskip.List(), nullptr,
                             nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Clip(method_lyr.get(), &res, opt_noskip.List(), nullptr,
                           nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              in_lyr->Erase(method_lyr.get(), &res, opt_noskip.List(), nullptr,
                            nullptr));

    CPLStringList opt_skip;
    opt_skip.AddString("SKIP_FAILURES=YES");
    EXPECT_EQ(OGRERR_NONE, in_lyr->Union(method_lyr.get(), &res,
                                         opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE,
              in_lyr->Intersection(method_lyr.get(), &res, opt_skip.List(),
                                   nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE,
              in_lyr->SymDifference(method_lyr.get(), &res, opt_skip.List(),
                                    nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE, in_lyr->Identity(method_lyr.get(), &res,
                                            opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE, in_lyr->Update(method_lyr.get(), &res,
                                          opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE, in_lyr->Clip(method_lyr.get(), &res, opt_skip.List(),
                                        nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE, in_lyr->Erase(method_lyr.get(), &res,
                                         opt_skip.List(), nullptr, nullptr));
  }

  // Self-intersecting bowtie polygon to trigger GEOS errors
  {
    auto bowtie_in = std::make_unique<UnfilteredTestLayer>("bt_in");
    auto bowtie_feat = std::make_unique<OGRFeature>(bowtie_in->GetLayerDefn());
    auto poly_bt = std::make_unique<OGRPolygon>();
    auto ring_bt = std::make_unique<OGRLinearRing>();
    ring_bt->addPoint(0, 0);
    ring_bt->addPoint(10, 10);
    ring_bt->addPoint(10, 0);
    ring_bt->addPoint(0, 10);
    ring_bt->addPoint(0, 0);
    poly_bt->addRingDirectly(ring_bt.release());
    bowtie_feat->SetGeometryDirectly(poly_bt.release());
    bowtie_in->AddFeature(std::move(bowtie_feat));
    bowtie_in->SetSpatialFilter(sf.get());

    UnfilteredTestLayer res_bt("res_bt");
    CPLStringList opt_skip;
    opt_skip.AddString("SKIP_FAILURES=YES");
    opt_skip.AddString("USE_PREPARED_GEOMETRIES=NO");
    EXPECT_EQ(OGRERR_NONE, bowtie_in->Union(method_lyr.get(), &res_bt,
                                            opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE,
              bowtie_in->Intersection(method_lyr.get(), &res_bt,
                                      opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE,
              bowtie_in->SymDifference(method_lyr.get(), &res_bt,
                                       opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE,
              bowtie_in->Identity(method_lyr.get(), &res_bt, opt_skip.List(),
                                  nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE,
              bowtie_in->Update(method_lyr.get(), &res_bt, opt_skip.List(),
                                nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE, bowtie_in->Clip(method_lyr.get(), &res_bt,
                                           opt_skip.List(), nullptr, nullptr));
    EXPECT_EQ(OGRERR_NONE, bowtie_in->Erase(method_lyr.get(), &res_bt,
                                            opt_skip.List(), nullptr, nullptr));

    CPLStringList opt_noskip;
    opt_noskip.AddString("SKIP_FAILURES=NO");
    opt_noskip.AddString("USE_PREPARED_GEOMETRIES=NO");
    {
      WithQuietHandler quiet_handler;
      EXPECT_NE(OGRERR_NONE,
                bowtie_in->Union(method_lyr.get(), &res_bt, opt_noskip.List(),
                                 nullptr, nullptr));
      EXPECT_NE(OGRERR_NONE,
                bowtie_in->Intersection(method_lyr.get(), &res_bt,
                                        opt_noskip.List(), nullptr, nullptr));
      CPLErrorReset();
    }
  }

  OSRCleanup();
}

TEST(LayerTest, RemainingCWrappersAndHelpers) {
  auto layer = std::make_unique<ScanTestLayer>("helpers_layer", wkbPolygon);
  OGRFeatureDefn* defn = const_cast<OGRFeatureDefn*>(layer->GetLayerDefn());

  OGRFieldDefn f("f", OFTString);
  defn->AddFieldDefn(&f);

  auto feat = std::make_unique<OGRFeature>(defn);
  feat->SetField(0, "val");
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  feat->SetGeometryDirectly(pt.release());
  layer->AddFeature(std::move(feat));

  // OGR_L_GetNextFeature & OGR_L_ResetReading
  OGR_L_ResetReading(layer.get());
  OGRFeatureH hFeat = OGR_L_GetNextFeature(layer.get());
  EXPECT_NE(nullptr, hFeat);
  OGR_F_Destroy(hFeat);

  // Rename & OGR_L_Rename
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, layer->Rename("new_name"));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_L_Rename(layer.get(), "new_name"));
  EXPECT_EQ(OGRERR_FAILURE, OGR_L_Rename(layer.get(), nullptr));

  // SetSpatialFilterEx & SetSpatialFilterRectEx via C API
  auto poly = std::make_unique<OGRPolygon>();
  OGR_L_SetSpatialFilterEx(layer.get(), 0, poly.get());
  OGR_L_SetSpatialFilterRect(layer.get(), 0, 0, 10, 10);
  OGR_L_SetSpatialFilterRectEx(layer.get(), 0, 0, 0, 10, 10);
  OGR_L_SetSpatialFilterEx(layer.get(), 0, nullptr);

  // Non-linear geometry conversion in OGR_L_GetGeomType
  {
    auto curve_layer =
        std::make_unique<ScanTestLayer>("curve_lyr", wkbCircularString);
    EXPECT_EQ(wkbCircularString, OGR_L_GetGeomType(curve_layer.get()));
    struct FlagRestorer {
      ~FlagRestorer() { OGRSetNonLinearGeometriesEnabledFlag(TRUE); }
    } restore_flag;
    OGRSetNonLinearGeometriesEnabledFlag(FALSE);
    EXPECT_EQ(wkbLineString, OGR_L_GetGeomType(curve_layer.get()));
  }

  // SupportedSRSList C API
  {
    auto srs_layer = std::make_unique<ScanTestLayer>("srs_lyr");
    int count = 0;
    EXPECT_EQ(nullptr, OGR_L_GetSupportedSRSList(srs_layer.get(), 0, &count));
    EXPECT_EQ(0, count);

    auto srs = std::make_unique<OGRSpatialReference>();
    srs->importFromEPSG(4326);
    srs_layer->AddSupportedSRS(std::move(srs));
    OGRSpatialReferenceH* pahSRS =
        OGR_L_GetSupportedSRSList(srs_layer.get(), 0, &count);
    ASSERT_NE(nullptr, pahSRS);
    EXPECT_EQ(1, count);
    OSRFreeSRSArray(pahSRS);
  }

  // ValidateGeometryFieldIndexForSetSpatialFilter and no-geom checks
  {
    auto no_geom_layer = std::make_unique<ScanTestLayer>("no_geom", wkbNone);
    EXPECT_TRUE(no_geom_layer->ValidateGeometryFieldIndexForSetSpatialFilter(
        0, nullptr, false));
    EXPECT_FALSE(no_geom_layer->ValidateGeometryFieldIndexForSetSpatialFilter(
        0, poly.get(), false));
    EXPECT_FALSE(no_geom_layer->ValidateGeometryFieldIndexForSetSpatialFilter(
        0, poly.get(), true));
    EXPECT_FALSE(no_geom_layer->ValidateGeometryFieldIndexForSetSpatialFilter(
        5, poly.get(), false));
    EXPECT_EQ(OGRERR_FAILURE, no_geom_layer->SetSpatialFilter(0, poly.get()));
    EXPECT_EQ(OGRERR_FAILURE, no_geom_layer->SetSpatialFilter(5, poly.get()));
    EXPECT_EQ(nullptr, no_geom_layer->GetSpatialRef());
    EXPECT_STREQ("", no_geom_layer->GetGeometryColumn());
  }

  // Null feature defn GetGeomType
  {
    class NullDefnLayer : public OGRLayer {
     public:
      const OGRFeatureDefn* GetLayerDefn() const override { return nullptr; }
      void ResetReading() override {}
      OGRFeature* GetNextFeature() override { return nullptr; }
      int TestCapability(const char*) const override { return FALSE; }
    };
    NullDefnLayer null_defn_lyr;
    EXPECT_EQ(wkbUnknown, null_defn_lyr.GetGeomType());
  }

  // Concurrent FeatureIterator error
  {
    auto it1 = layer->begin();
    auto it2 = layer->begin();
    EXPECT_TRUE(it1 != it2);
  }

  // InitializeIndexSupport (first and second call)
  layer->InitializeIndexSupport("/vsimem/dummy.idx");
  layer->InitializeIndexSupport("/vsimem/dummy.idx");

  // SetAttributeFilter special geometry fields & clear
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("OGR_GEOMETRY IS NOT NULL"));
  EXPECT_TRUE(layer->AttributeFilterEvaluationNeedsGeometry());
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("OGR_GEOM_WKT IS NOT NULL"));
  EXPECT_TRUE(layer->AttributeFilterEvaluationNeedsGeometry());
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("OGR_GEOM_AREA > 0"));
  EXPECT_TRUE(layer->AttributeFilterEvaluationNeedsGeometry());
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("f = 'val'"));
  EXPECT_FALSE(layer->AttributeFilterEvaluationNeedsGeometry());
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter(nullptr));
  EXPECT_FALSE(layer->AttributeFilterEvaluationNeedsGeometry());

  // SetIgnoredFields with unrecognized field and OGR_GEOMETRY
  const char* ignored[] = {"f", "non_existent", nullptr};
  EXPECT_EQ(OGRERR_FAILURE, layer->SetIgnoredFields(ignored));
  const char* geom_ignored[] = {"OGR_GEOMETRY", nullptr};
  EXPECT_EQ(OGRERR_NONE, layer->SetIgnoredFields(geom_ignored));
  EXPECT_EQ(OGRERR_NONE, layer->SetIgnoredFields(nullptr));

  // BasicLayer destruction with style table and attr query
  {
    BasicLayer bl;
    bl.SetStyleTableDirectly(new OGRStyleTable());
    bl.SetAttributeFilter("f = 'test'");
  }

  // GetExtent3D with 2D features
  {
    auto lyr3d = std::make_unique<ScanTestLayer>("lyr3d", wkbPoint);
    OGRFeatureDefn* defn3d = const_cast<OGRFeatureDefn*>(lyr3d->GetLayerDefn());
    auto feat1 = std::make_unique<OGRFeature>(defn3d);
    feat1->SetGeometryDirectly(new OGRPoint(1, 2));
    lyr3d->AddFeature(std::move(feat1));
    auto feat2 = std::make_unique<OGRFeature>(defn3d);
    feat2->SetGeometryDirectly(new OGRPoint(3, 4));
    lyr3d->AddFeature(std::move(feat2));
    OGREnvelope3D env3d;
    EXPECT_EQ(OGRERR_NONE, lyr3d->GetExtent3D(0, &env3d, true));
  }

  // IUpdateFeature with updated geom field
  {
    auto updatable = std::make_unique<DelegatingUpdatableLayer>("upd_geom");
    OGRFeatureDefn* upd_defn =
        const_cast<OGRFeatureDefn*>(updatable->GetLayerDefn());
    upd_defn->SetGeomType(wkbPoint);
    OGRFeature feat_upd(upd_defn);
    feat_upd.SetFID(OGRNullFID);
    auto p1 = std::make_unique<OGRPoint>(1, 1);
    feat_upd.SetGeometryDirectly(p1.release());
    EXPECT_EQ(OGRERR_NONE, updatable->CreateFeature(&feat_upd));

    OGRFeature feat_mod(upd_defn);
    feat_mod.SetFID(feat_upd.GetFID());
    auto p2 = std::make_unique<OGRPoint>(2, 2);
    feat_mod.SetGeometryDirectly(p2.release());
    int geom_idx = 0;
    EXPECT_EQ(OGRERR_NONE, updatable->UpdateFeature(&feat_mod, 0, nullptr, 1,
                                                    &geom_idx, false));
  }

  OSRCleanup();
}

// TODO: b/335317901 - Cover remaining error branches in ogrlayer.cpp.

}  // namespace
}  // namespace autotest2
