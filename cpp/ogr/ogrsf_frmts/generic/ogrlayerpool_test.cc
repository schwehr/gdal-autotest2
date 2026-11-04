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

// Tests for OGRLayerPool and OGRProxiedLayer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrlayerpool.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerpool.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerpool.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

struct LayerUserData {
  std::string name;
  bool opened = false;
  bool freed = false;
};

OGRLayer* OpenMemLayer(void* user_data) {
  auto* ud = static_cast<LayerUserData*>(user_data);
  ud->opened = true;

  auto srs = std::make_unique<OGRSpatialReference>();
  srs->importFromEPSG(4326);

  auto* layer = new OGRMemLayer(ud->name.c_str(), srs.get(), wkbPoint);
  OGRFieldDefn fld("val", OFTInteger);
  layer->CreateField(&fld);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField(0, 42);
  auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
  feat->SetGeometry(pt.get());
  CPL_IGNORE_RET_VAL(layer->CreateFeature(feat.get()));

  return layer;
}

void ReleaseMemLayer(OGRLayer* layer, void* user_data) {
  auto* ud = static_cast<LayerUserData*>(user_data);
  ud->opened = false;
  delete layer;
}

void FreeUserData(void* user_data) {
  auto* ud = static_cast<LayerUserData*>(user_data);
  ud->freed = true;
}

OGRLayer* FailingOpenLayer(void*) { return nullptr; }

TEST(OGRLayerPoolTest, PoolEvictionAndChaining) {
  OGRLayerPool pool(2);
  EXPECT_EQ(2, pool.GetMaxSimultaneouslyOpened());
  EXPECT_EQ(0, pool.GetSize());

  LayerUserData ud1{"layer1"};
  LayerUserData ud2{"layer2"};
  LayerUserData ud3{"layer3"};

  auto proxied1 = std::make_unique<OGRProxiedLayer>(
      &pool, OpenMemLayer, ReleaseMemLayer, FreeUserData, &ud1);
  auto proxied2 = std::make_unique<OGRProxiedLayer>(
      &pool, OpenMemLayer, ReleaseMemLayer, FreeUserData, &ud2);
  auto proxied3 = std::make_unique<OGRProxiedLayer>(
      &pool, OpenMemLayer, ReleaseMemLayer, FreeUserData, &ud3);

  // Access layer 1
  EXPECT_EQ(1, proxied1->GetFeatureCount());
  EXPECT_TRUE(ud1.opened);
  EXPECT_EQ(1, pool.GetSize());

  // Access layer 2
  EXPECT_EQ(1, proxied2->GetFeatureCount());
  EXPECT_TRUE(ud2.opened);
  EXPECT_EQ(2, pool.GetSize());

  // Re-access layer 1 (middle of chain move to MRU)
  EXPECT_EQ(1, proxied1->GetFeatureCount());
  EXPECT_TRUE(ud1.opened);
  EXPECT_EQ(2, pool.GetSize());

  // Access layer 3 (triggers LRU eviction of layer 1)
  EXPECT_EQ(1, proxied3->GetFeatureCount());
  EXPECT_TRUE(ud3.opened);
  EXPECT_FALSE(ud1.opened);  // layer 1 was evicted!
  EXPECT_TRUE(ud2.opened);   // layer 2 is still open!
  EXPECT_EQ(2, pool.GetSize());

  // Destroying proxied layer calls FreeUserData
  proxied1.reset();
  EXPECT_TRUE(ud1.freed);
}

TEST(OGRLayerPoolTest, ProxiedLayerAllMethods) {
  OGRLayerPool pool(5);
  LayerUserData ud{"full_test"};

  // Use 4-argument constructor
  auto proxied =
      std::make_unique<OGRProxiedLayer>(&pool, OpenMemLayer, FreeUserData, &ud);

  EXPECT_STREQ("full_test", proxied->GetName());
  EXPECT_EQ(wkbPoint, proxied->GetGeomType());
  EXPECT_NE(nullptr, proxied->GetLayerDefn());
  EXPECT_NE(nullptr, proxied->GetSpatialRef());
  EXPECT_EQ(1, proxied->GetFeatureCount());
  EXPECT_NE(nullptr, proxied->GetUnderlyingLayer());
  EXPECT_EQ(nullptr, proxied->GetDataset());

  // Spatial & Attribute filter
  OGRPoint filter_pt(10.0, 20.0);
  EXPECT_EQ(OGRERR_NONE, proxied->ISetSpatialFilter(0, &filter_pt));
  EXPECT_NE(nullptr, proxied->GetSpatialFilter());
  EXPECT_EQ(OGRERR_NONE, proxied->ISetSpatialFilter(0, nullptr));
  EXPECT_EQ(nullptr, proxied->GetSpatialFilter());

  EXPECT_EQ(OGRERR_NONE, proxied->SetAttributeFilter("val = 42"));
  EXPECT_EQ(OGRERR_NONE, proxied->SetAttributeFilter(nullptr));

  // Features
  proxied->ResetReading();
  OGRFeature* feat = proxied->GetNextFeature();
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(42, feat->GetFieldAsInteger(0));
  OGRFeature::DestroyFeature(feat);

  EXPECT_EQ(OGRERR_NONE, proxied->SetNextByIndex(0));
  OGRFeature* by_fid = proxied->GetFeature(0);
  ASSERT_NE(nullptr, by_fid);
  EXPECT_EQ(OGRERR_NONE, proxied->ISetFeature(by_fid));
  OGRFeature::DestroyFeature(by_fid);

  {
    auto new_feat = std::make_unique<OGRFeature>(proxied->GetLayerDefn());
    new_feat->SetFID(1);
    new_feat->SetField(0, 99);
    EXPECT_EQ(OGRERR_NONE, proxied->ICreateFeature(new_feat.get()));
    EXPECT_EQ(OGRERR_NONE, proxied->IUpsertFeature(new_feat.get()));
    int updated = 0;
    EXPECT_EQ(OGRERR_NONE, proxied->IUpdateFeature(new_feat.get(), 1, &updated,
                                                   0, nullptr, false));
  }

  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, proxied->IGetExtent(0, &env, true));

  ArrowArrayStream stream;
  EXPECT_TRUE(proxied->GetArrowStream(&stream, nullptr));
  if (stream.release) {
    stream.release(&stream);
  }

  EXPECT_TRUE(proxied->TestCapability(OLCRandomRead));
  OGRFieldDefn extra_fld("extra", OFTString);
  EXPECT_EQ(OGRERR_NONE, proxied->CreateField(&extra_fld));
  EXPECT_EQ(OGRERR_NONE, proxied->DeleteField(1));
  int map[] = {0};
  EXPECT_EQ(OGRERR_NONE, proxied->ReorderFields(map));
  EXPECT_EQ(OGRERR_NONE,
            proxied->AlterFieldDefn(0, &extra_fld, ALTER_NAME_FLAG));
  OGRGeomFieldDefn geom_extra("geom_extra", wkbPoint);
  EXPECT_EQ(OGRERR_NONE,
            proxied->AlterGeomFieldDefn(0, &geom_extra, ALTER_NAME_FLAG));

  EXPECT_EQ(OGRERR_NONE, proxied->SyncToDisk());

  EXPECT_EQ(nullptr, proxied->GetStyleTable());
  proxied->SetStyleTableDirectly(new OGRStyleTable());
  EXPECT_NE(nullptr, proxied->GetStyleTable());
  OGRStyleTable tbl;
  proxied->SetStyleTable(&tbl);

  EXPECT_EQ(OGRERR_NONE, proxied->StartTransaction());
  EXPECT_EQ(OGRERR_NONE, proxied->CommitTransaction());
  EXPECT_EQ(OGRERR_NONE, proxied->StartTransaction());
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, proxied->RollbackTransaction());

  EXPECT_STREQ("", proxied->GetFIDColumn());
  EXPECT_STREQ("", proxied->GetGeometryColumn());
  const char* ignored[] = {"val", nullptr};
  EXPECT_EQ(OGRERR_FAILURE, proxied->SetIgnoredFields(ignored));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, proxied->Rename("renamed_proxied"));

  EXPECT_EQ(OGRERR_NONE, proxied->DeleteFeature(0));
  EXPECT_EQ(OGRERR_NONE, proxied->DeleteFeature(1));
}

TEST(OGRLayerPoolTest, FailingOpenLayerGuards) {
  OGRLayerPool pool(2);
  OGRProxiedLayer failing_layer(&pool, FailingOpenLayer, nullptr, nullptr);

  EXPECT_EQ(nullptr, failing_layer.GetUnderlyingLayer());
  EXPECT_EQ(nullptr, failing_layer.GetSpatialFilter());
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.ISetSpatialFilter(0, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.SetAttributeFilter("1=1"));
  failing_layer.ResetReading();
  EXPECT_EQ(nullptr, failing_layer.GetNextFeature());
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.SetNextByIndex(0));
  EXPECT_EQ(nullptr, failing_layer.GetFeature(0));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.ISetFeature(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.ICreateFeature(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.IUpsertFeature(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.IUpdateFeature(nullptr, 0, nullptr, 0,
                                                         nullptr, false));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.DeleteFeature(0));
  EXPECT_EQ(nullptr, failing_layer.GetDataset());

  ArrowArrayStream stream;
  EXPECT_FALSE(failing_layer.GetArrowStream(&stream, nullptr));

  EXPECT_STREQ("", failing_layer.GetName());
  EXPECT_EQ(wkbUnknown, failing_layer.GetGeomType());
  EXPECT_NE(nullptr, failing_layer.GetLayerDefn());
  EXPECT_STREQ("", failing_layer.GetLayerDefn()->GetName());
  EXPECT_EQ(nullptr, failing_layer.GetSpatialRef());
  EXPECT_EQ(0, failing_layer.GetFeatureCount());
  OGREnvelope env;
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.IGetExtent(0, &env, true));
  EXPECT_FALSE(failing_layer.TestCapability(OLCRandomRead));

  OGRFieldDefn fld("f", OFTInteger);
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.CreateField(&fld));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.DeleteField(0));
  int map[] = {0};
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.ReorderFields(map));
  EXPECT_EQ(OGRERR_FAILURE,
            failing_layer.AlterFieldDefn(0, &fld, ALTER_ALL_FLAG));
  OGRGeomFieldDefn gfld("g", wkbPoint);
  EXPECT_EQ(OGRERR_FAILURE,
            failing_layer.AlterGeomFieldDefn(0, &gfld, ALTER_ALL_FLAG));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.SyncToDisk());

  EXPECT_EQ(nullptr, failing_layer.GetStyleTable());
  failing_layer.SetStyleTableDirectly(nullptr);
  failing_layer.SetStyleTable(nullptr);

  EXPECT_EQ(OGRERR_FAILURE, failing_layer.StartTransaction());
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.CommitTransaction());
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.RollbackTransaction());

  EXPECT_STREQ("", failing_layer.GetFIDColumn());
  EXPECT_STREQ("", failing_layer.GetGeometryColumn());
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.SetIgnoredFields(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, failing_layer.Rename("fail"));
}

// TODO: b/335317901 - Cover remaining error branches in ogrlayerpool.cpp.

}  // namespace
}  // namespace autotest2
