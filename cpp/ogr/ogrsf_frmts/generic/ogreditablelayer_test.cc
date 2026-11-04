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

// Tests for OGREditableLayer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogreditablelayer.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogreditablelayer.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogreditablelayer.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

class TestSynchronizer : public IOGREditableLayerSynchronizer {
 public:
  ~TestSynchronizer() override = default;
  OGRErr EditableSyncToDisk(OGRLayer*, OGRLayer**) override {
    return OGRERR_NONE;
  }
};

TEST(OGREditableLayerTest, BufferedEditsOnNonUpdatableLayer) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto mem_layer = std::make_unique<OGRMemLayer>("base_layer", &srs, wkbPoint);

  OGRFieldDefn f_int("f_int", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, mem_layer->CreateField(&f_int));

  OGRFieldDefn f_str("f_str", OFTString);
  ASSERT_EQ(OGRERR_NONE, mem_layer->CreateField(&f_str));

  {
    auto feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    feat->SetFID(5);
    feat->SetField(0, 10);
    feat->SetField(1, "str10");
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(feat.get()));
  }

  // Make base layer non-updatable so edits are buffered in OGREditableLayer's
  // m_poMemLayer
  mem_layer->SetUpdatable(false);

  OGREditableLayer editable(
      mem_layer.get(), /*bTakeOwnershipDecoratedLayer=*/false,
      new TestSynchronizer(), /*bTakeOwnershipSynchronizer=*/true);

  editable.SetSupportsCreateGeomField(true);
  editable.SetSupportsCurveGeometries(true);

  EXPECT_STREQ("base_layer", editable.GetName());
  EXPECT_EQ(wkbPoint, editable.GetGeomType());
  EXPECT_NE(nullptr, editable.GetLayerDefn());
  EXPECT_NE(nullptr, editable.GetSpatialRef());
  EXPECT_EQ(1, editable.GetFeatureCount());

  EXPECT_EQ(OGRERR_NONE, editable.SetNextByIndex(0));
  {
    std::unique_ptr<OGRFeature> f_unedited(editable.GetFeature(5));
    ASSERT_NE(nullptr, f_unedited);
    EXPECT_EQ(nullptr, editable.GetFeature(9999));

    // Edit existing feature 5 (puts 5 into m_oSetEdited)
    f_unedited->SetField(1, "edited_str");
    EXPECT_EQ(OGRERR_NONE, editable.SetFeature(f_unedited.get()));
  }

  // Schema alterations
  OGRFieldDefn f_extra("f_extra", OFTReal);
  EXPECT_EQ(OGRERR_NONE, editable.CreateField(&f_extra));

  OGRGeomFieldDefn gfld_extra("geom_extra", wkbPoint);
  EXPECT_EQ(OGRERR_NONE, editable.CreateGeomField(&gfld_extra));

  int map[] = {1, 0, 2};
  EXPECT_EQ(OGRERR_NONE, editable.ReorderFields(map));

  EXPECT_EQ(OGRERR_NONE, editable.DeleteField(2));

  OGRFieldDefn f_renamed("f_renamed", OFTString);
  EXPECT_EQ(OGRERR_NONE,
            editable.AlterFieldDefn(0, &f_renamed, ALTER_NAME_FLAG));

  OGRGeomFieldDefn gfld_renamed("geom_renamed", wkbPoint);
  EXPECT_EQ(OGRERR_NONE,
            editable.AlterGeomFieldDefn(0, &gfld_renamed, ALTER_NAME_FLAG));

  // Feature operations (CreateFeature runs DetectNextFID because m_nNextFID <
  // 0)
  {
    auto feat = std::make_unique<OGRFeature>(editable.GetLayerDefn());
    feat->SetField(0, "created");
    feat->SetField(1, 99);
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    feat->SetGeometry(pt.get());
    EXPECT_EQ(OGRERR_NONE, editable.ICreateFeature(feat.get()));
    EXPECT_EQ(6, feat->GetFID());

    // Update created feature
    feat->SetField(1, 101);
    EXPECT_EQ(OGRERR_NONE, editable.ISetFeature(feat.get()));
    EXPECT_EQ(OGRERR_NONE, editable.IUpsertFeature(feat.get()));

    int updated = 1;
    EXPECT_EQ(OGRERR_NONE, editable.IUpdateFeature(feat.get(), 1, &updated, 0,
                                                   nullptr, false));
  }

  EXPECT_EQ(2, editable.GetFeatureCount());

  // Reading features with m_oSetEdited and m_oSetCreated
  editable.ResetReading();
  OGRFeature* read1 = editable.GetNextFeature();
  ASSERT_NE(nullptr, read1);
  EXPECT_EQ(5, read1->GetFID());
  OGRFeature::DestroyFeature(read1);

  OGRFeature* read2 = editable.GetNextFeature();
  ASSERT_NE(nullptr, read2);
  EXPECT_EQ(6, read2->GetFID());
  OGRFeature::DestroyFeature(read2);

  // Filters during iteration
  editable.SetAttributeFilter("1=0");
  editable.ResetReading();
  EXPECT_EQ(nullptr, editable.GetNextFeature());
  editable.SetAttributeFilter(nullptr);

  editable.SetSpatialFilterRect(100, 100, 200, 200);
  editable.ResetReading();
  EXPECT_EQ(nullptr, editable.GetNextFeature());
  editable.SetSpatialFilter(nullptr);

  EXPECT_EQ(OGRERR_NONE, editable.SetNextByIndex(1));
  OGRFeature* by_idx = editable.GetNextFeature();
  ASSERT_NE(nullptr, by_idx);
  EXPECT_EQ(6, by_idx->GetFID());
  OGRFeature::DestroyFeature(by_idx);

  OGRFeature* by_fid = editable.GetFeature(6);
  ASSERT_NE(nullptr, by_fid);
  OGRFeature::DestroyFeature(by_fid);

  // Extent with edited features
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, editable.IGetExtent(0, &env, true));

  OGRPoint filter_pt(10.0, 20.0);
  EXPECT_EQ(OGRERR_NONE, editable.ISetSpatialFilter(0, &filter_pt));
  EXPECT_NE(nullptr, editable.GetSpatialFilter());
  EXPECT_EQ(OGRERR_NONE, editable.ISetSpatialFilter(0, nullptr));

  EXPECT_EQ(OGRERR_NONE, editable.SetAttributeFilter("1=1"));
  EXPECT_EQ(OGRERR_NONE, editable.SetAttributeFilter(nullptr));

  // ArrowStream
  ArrowArrayStream stream;
  EXPECT_TRUE(editable.GetArrowStream(&stream, nullptr));
  if (stream.release) {
    stream.release(&stream);
  }

  // Capabilities & Transactions
  EXPECT_TRUE(editable.TestCapability(OLCRandomRead));
  EXPECT_TRUE(editable.TestCapability(OLCRandomWrite));
  EXPECT_TRUE(editable.TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(editable.TestCapability(OLCDeleteFeature));
  EXPECT_TRUE(editable.TestCapability(OLCCreateField));
  EXPECT_TRUE(editable.TestCapability(OLCDeleteField));
  EXPECT_TRUE(editable.TestCapability(OLCReorderFields));
  EXPECT_TRUE(editable.TestCapability(OLCAlterFieldDefn));
  EXPECT_TRUE(editable.TestCapability(OLCCreateGeomField));
  EXPECT_TRUE(editable.TestCapability(OLCCurveGeometries));
  EXPECT_FALSE(editable.TestCapability(OLCTransactions));

  EXPECT_STREQ("", editable.GetGeometryColumn());

  EXPECT_EQ(OGRERR_NONE, editable.StartTransaction());
  EXPECT_EQ(OGRERR_NONE, editable.CommitTransaction());
  EXPECT_EQ(OGRERR_NONE, editable.StartTransaction());
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, editable.RollbackTransaction());

  // DeleteFeature: test all internal branches before SyncToDisk clears sets
  // 1. Delete edited feature (FID 5 in m_oSetEdited)
  EXPECT_EQ(OGRERR_NONE, editable.DeleteFeature(5));
  EXPECT_EQ(nullptr, editable.GetFeature(5));
  // 2. Delete already deleted feature (FID 5 in m_oSetDeleted)
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, editable.DeleteFeature(5));
  }
  // 3. Delete created feature (FID 6 in m_oSetCreated)
  EXPECT_EQ(OGRERR_NONE, editable.DeleteFeature(6));
  EXPECT_EQ(0, editable.GetFeatureCount());

  // SyncToDisk with modified structures and deleted features
  EXPECT_EQ(OGRERR_NONE, editable.SyncToDisk());
  // SyncToDisk when clean (early return branch)
  EXPECT_EQ(OGRERR_NONE, editable.SyncToDisk());

  // Now that m_oSetDeleted was cleared, deleting FID 5 finds it in base layer
  EXPECT_EQ(OGRERR_NONE, editable.DeleteFeature(5));
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, editable.DeleteFeature(9999));
  }
}

class UpdatableGeomMemLayer : public OGRMemLayer {
 public:
  UpdatableGeomMemLayer() : OGRMemLayer("updatable", nullptr, wkbNone) {}
  int TestCapability(const char* pszCap) const override {
    if (EQUAL(pszCap, OLCCreateGeomField)) return TRUE;
    return OGRMemLayer::TestCapability(pszCap);
  }
};

TEST(OGREditableLayerTest, DirectPassThroughOnUpdatableLayer) {
  auto mem_layer = std::make_unique<UpdatableGeomMemLayer>();

  OGREditableLayer editable(mem_layer.get(),
                            /*bTakeOwnershipDecoratedLayer=*/false, nullptr,
                            /*bTakeOwnershipSynchronizer=*/false);
  editable.SetSupportsCreateGeomField(true);

  // Direct pass-through CreateGeomField
  OGRGeomFieldDefn gfld("geom1", wkbPoint);
  EXPECT_EQ(OGRERR_NONE, editable.CreateGeomField(&gfld));

  // Direct pass-through CreateFeature & SetFeature before modifying structure
  {
    auto f = std::make_unique<OGRFeature>(editable.GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    f->SetGeomFieldDirectly(0, pt.release());
    EXPECT_EQ(OGRERR_NONE, editable.CreateFeature(f.get()));
    GIntBig fid = f->GetFID();
    EXPECT_GE(fid, 0);

    EXPECT_EQ(OGRERR_NONE, editable.SetFeature(f.get()));
  }

  // Now modify structure
  OGRFieldDefn fld("f1", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, editable.CreateField(&fld));

  OGRFieldDefn fld2("f2", OFTString);
  EXPECT_EQ(OGRERR_NONE, editable.CreateField(&fld2));

  int map[] = {1, 0};
  EXPECT_EQ(OGRERR_NONE, editable.ReorderFields(map));

  EXPECT_EQ(OGRERR_NONE, editable.DeleteField(1));
}

// TODO: b/335317901 - Cover remaining error branches in ogreditablelayer.cpp.

}  // namespace
}  // namespace autotest2
