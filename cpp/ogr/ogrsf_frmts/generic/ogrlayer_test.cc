// Copyright 2017 Google Inc. All Rights Reserved.
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

// Test OGRFeature.

#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_port.h"

#include <memory>

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"

namespace autotest2 {
namespace {

constexpr char kLayerName[] = "basic layer";

class BasicLayer : public OGRLayer {
 private:
  std::unique_ptr<OGRFeatureDefn> feature_defn_;

 public:
  BasicLayer() : feature_defn_(absl::MakeUnique<OGRFeatureDefn>(kLayerName)) {}
  void ResetReading() override {}
  OGRFeature *GetNextFeature() override { return nullptr; }
  OGRFeatureDefn *GetLayerDefn() override { return feature_defn_.get(); }
  int TestCapability(const char *) override { return FALSE; }
};

TEST(LayerTest, FeatureCount) {
  auto layer = absl::MakeUnique<BasicLayer>();

  EXPECT_EQ(-1, layer->GetFeatureCount(FALSE));
  EXPECT_EQ(0, layer->GetFeatureCount(TRUE));
  EXPECT_EQ(-1, OGR_L_GetFeatureCount(layer.get(), FALSE));
  EXPECT_EQ(0, OGR_L_GetFeatureCount(layer.get(), TRUE));
}

TEST(LayerTest, GetExtent) {
  auto layer = absl::MakeUnique<BasicLayer>();
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
  auto layer = absl::MakeUnique<BasicLayer>();
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
  auto layer = absl::MakeUnique<BasicLayer>();
  EXPECT_EQ(0, layer->GetRefCount());
  EXPECT_EQ(1, layer->Reference());
  EXPECT_EQ(1, layer->GetRefCount());
  EXPECT_EQ(0, layer->Dereference());
  EXPECT_EQ(-1, layer->Dereference());
}

TEST(LayerTest, ReferenceCountingC) {
  auto layer = absl::MakeUnique<BasicLayer>();
  EXPECT_EQ(0, OGR_L_GetRefCount(layer.get()));
  EXPECT_EQ(1, OGR_L_Reference(layer.get()));
  EXPECT_EQ(0, OGR_L_Dereference(layer.get()));
  EXPECT_EQ(-1, OGR_L_Dereference(layer.get()));
}

// TODO(schwehr): Test OGR_L_AlterFieldDefn
// TODO(schwehr): Test OGR_L_Clip
// TODO(schwehr): Test OGR_L_CommitTransaction
// TODO(schwehr): Test OGR_L_CreateFeature
// TODO(schwehr): Test OGR_L_CreateField
// TODO(schwehr): Test OGR_L_CreateGeomField
// TODO(schwehr): Test OGR_L_DeleteFeature
// TODO(schwehr): Test OGR_L_DeleteField
// TODO(schwehr): Test OGR_L_Erase
// TODO(schwehr): Test OGR_L_FindFieldIndex
// TODO(schwehr): Test OGR_L_GetFIDColumn
// TODO(schwehr): Test OGR_L_GetFeature
// TODO(schwehr): Test OGR_L_GetFeaturesRead
// TODO(schwehr): Test OGR_L_GetGeomType
// TODO(schwehr): Test OGR_L_GetGeometryColumn
// TODO(schwehr): Test OGR_L_GetLayerDefn
// TODO(schwehr): Test OGR_L_GetName
// TODO(schwehr): Test OGR_L_GetNextFeature
// TODO(schwehr): Test OGR_L_GetSpatialFilter
// TODO(schwehr): Test OGR_L_GetSpatialRef
// TODO(schwehr): Test OGR_L_GetStyleTable
// TODO(schwehr): Test OGR_L_Identity
// TODO(schwehr): Test OGR_L_Intersection
// TODO(schwehr): Test OGR_L_ReorderField
// TODO(schwehr): Test OGR_L_ReorderFields
// TODO(schwehr): Test OGR_L_ResetReading
// TODO(schwehr): Test OGR_L_RollbackTransaction
// TODO(schwehr): Test OGR_L_SetAttributeFilter
// TODO(schwehr): Test OGR_L_SetFeature
// TODO(schwehr): Test OGR_L_SetIgnoredFields
// TODO(schwehr): Test OGR_L_SetNextByIndex
// TODO(schwehr): Test OGR_L_SetSpatialFilter
// TODO(schwehr): Test OGR_L_SetSpatialFilterEx
// TODO(schwehr): Test OGR_L_SetSpatialFilterRect
// TODO(schwehr): Test OGR_L_SetSpatialFilterRectEx
// TODO(schwehr): Test OGR_L_SetStyleTable
// TODO(schwehr): Test OGR_L_SetStyleTableDirectly
// TODO(schwehr): Test OGR_L_StartTransaction
// TODO(schwehr): Test OGR_L_SymDifference
// TODO(schwehr): Test OGR_L_SyncToDisk
// TODO(schwehr): Test OGR_L_TestCapability
// TODO(schwehr): Test OGR_L_Union
// TODO(schwehr): Test OGR_L_Update

}  // namespace
}  // namespace autotest2
