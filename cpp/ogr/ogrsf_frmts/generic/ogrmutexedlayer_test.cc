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

// Tests for OGRMutexedLayer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrmutexedlayer.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrmutexedlayer.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrmutexedlayer.h"

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/port/cpl_multiproc.h"

namespace autotest2 {
namespace {

TEST(OGRMutexedLayerTest, AllMethodsWithMutex) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  auto mem_layer = std::make_unique<OGRMemLayer>("mutex_layer", &srs, wkbPoint);

  OGRFieldDefn field_int("int_fld", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, mem_layer->CreateField(&field_int));

  {
    auto feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    feat->SetField(0, 100);
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(feat.get()));
  }

  CPLMutex* mutex = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex);
  CPLReleaseMutex(
      mutex);  // CPLCreateMutex acquires the lock; release it for use.
  std::unique_ptr<CPLMutex, void (*)(CPLMutex*)> mutex_guard(mutex,
                                                             CPLDestroyMutex);

  {
    OGRMutexedLayer mlayer(mem_layer.get(), /*bTakeOwnership=*/false, mutex);

    EXPECT_STREQ("mutex_layer", mlayer.GetName());
    EXPECT_EQ(wkbPoint, mlayer.GetGeomType());
    EXPECT_NE(nullptr, mlayer.GetLayerDefn());
    EXPECT_NE(nullptr, mlayer.GetSpatialRef());
    EXPECT_EQ(1, mlayer.GetFeatureCount());
    EXPECT_EQ(mem_layer->GetDataset(), mlayer.GetDataset());

    OGRPoint filter_pt(1.0, 2.0);
    EXPECT_EQ(OGRERR_NONE, mlayer.ISetSpatialFilter(0, &filter_pt));
    EXPECT_NE(nullptr, mlayer.GetSpatialFilter());
    EXPECT_EQ(OGRERR_NONE, mlayer.ISetSpatialFilter(0, nullptr));
    EXPECT_EQ(nullptr, mlayer.GetSpatialFilter());

    EXPECT_EQ(OGRERR_NONE, mlayer.SetAttributeFilter("int_fld = 100"));
    EXPECT_EQ(OGRERR_NONE, mlayer.SetAttributeFilter(nullptr));

    mlayer.ResetReading();
    std::unique_ptr<OGRFeature> read_feat(mlayer.GetNextFeature());
    ASSERT_NE(nullptr, read_feat);
    EXPECT_EQ(100, read_feat->GetFieldAsInteger(0));

    EXPECT_EQ(OGRERR_NONE, mlayer.SetNextByIndex(0));
    std::unique_ptr<OGRFeature> by_fid(mlayer.GetFeature(0));
    ASSERT_NE(nullptr, by_fid);
    EXPECT_EQ(OGRERR_NONE, mlayer.ISetFeature(by_fid.get()));

    {
      auto new_feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
      new_feat->SetFID(1);
      new_feat->SetField(0, 99);
      EXPECT_EQ(OGRERR_NONE, mlayer.ICreateFeature(new_feat.get()));
      EXPECT_EQ(OGRERR_NONE, mlayer.IUpsertFeature(new_feat.get()));
      int updated_field = 0;
      EXPECT_EQ(OGRERR_NONE,
                mlayer.IUpdateFeature(new_feat.get(), 1, &updated_field, 0,
                                      nullptr, false));
    }

    OGREnvelope envelope;
    EXPECT_EQ(OGRERR_NONE, mlayer.IGetExtent(0, &envelope, true));

    ArrowArrayStream stream;
    EXPECT_TRUE(mlayer.GetArrowStream(&stream, nullptr));
    if (stream.release) {
      stream.release(&stream);
    }

    EXPECT_TRUE(mlayer.TestCapability(OLCRandomRead));
    OGRFieldDefn field_new("extra", OFTString);
    EXPECT_EQ(OGRERR_NONE, mlayer.CreateField(&field_new));
    EXPECT_EQ(OGRERR_NONE, mlayer.DeleteField(1));
    int map[] = {0};
    EXPECT_EQ(OGRERR_NONE, mlayer.ReorderFields(map));
    EXPECT_EQ(OGRERR_NONE,
              mlayer.AlterFieldDefn(0, &field_int, ALTER_NAME_FLAG));
    OGRGeomFieldDefn geom_new("geom_extra", wkbPoint);
    EXPECT_EQ(OGRERR_NONE,
              mlayer.AlterGeomFieldDefn(0, &geom_new, ALTER_NAME_FLAG));

    EXPECT_EQ(OGRERR_NONE, mlayer.SyncToDisk());

    EXPECT_EQ(nullptr, mlayer.GetStyleTable());
    mlayer.SetStyleTableDirectly(new OGRStyleTable());
    EXPECT_NE(nullptr, mlayer.GetStyleTable());
    OGRStyleTable tbl;
    mlayer.SetStyleTable(&tbl);

    EXPECT_EQ(OGRERR_NONE, mlayer.StartTransaction());
    EXPECT_EQ(OGRERR_NONE, mlayer.CommitTransaction());
    EXPECT_EQ(OGRERR_NONE, mlayer.StartTransaction());
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, mlayer.RollbackTransaction());

    EXPECT_STREQ("", mlayer.GetFIDColumn());
    EXPECT_STREQ("", mlayer.GetGeometryColumn());
    const char* ignored[] = {"int_fld", nullptr};
    EXPECT_EQ(OGRERR_NONE, mlayer.SetIgnoredFields(ignored));

    EXPECT_EQ(nullptr, mlayer.GetMetadataDomainList());
    EXPECT_EQ(nullptr, mlayer.GetMetadata());
    char* meta[] = {nullptr};
    EXPECT_EQ(CE_None, mlayer.SetMetadata(meta));
    EXPECT_EQ(nullptr, mlayer.GetMetadataItem("item"));
    EXPECT_EQ(CE_None, mlayer.SetMetadataItem("item", "val"));
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
              mlayer.Rename("renamed_mutex_layer"));

    EXPECT_EQ(OGRERR_NONE, mlayer.DeleteFeature(0));
    EXPECT_EQ(OGRERR_NONE, mlayer.DeleteFeature(1));
  }
}

TEST(OGRMutexedLayerTest, WithoutMutexAndOwnership) {
  auto* mem_layer = new OGRMemLayer("null_mutex_layer", nullptr, wkbNone);
  {
    OGRMutexedLayer mlayer(mem_layer, /*bTakeOwnership=*/true,
                           /*hMutex=*/nullptr);
    EXPECT_STREQ("null_mutex_layer", mlayer.GetName());
    EXPECT_EQ(0, mlayer.GetFeatureCount());
  }  // destructor deletes mem_layer
}

// TODO: b/335317901 - Cover remaining error branches in ogrmutexedlayer.cpp.

}  // namespace
}  // namespace autotest2
