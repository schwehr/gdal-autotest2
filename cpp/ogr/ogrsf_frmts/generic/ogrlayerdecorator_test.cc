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

// Tests for OGRLayerDecorator.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrlayerdecorator.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerdecorator.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerdecorator.h"

#include <memory>
#include <vector>

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

class NullDecoratedLayer : public OGRLayerDecorator {
 public:
  explicit NullDecoratedLayer(OGRLayer* dummy)
      : OGRLayerDecorator(dummy, /*bTakeOwnership=*/false) {
    m_poDecoratedLayer = nullptr;
  }
};

TEST(OGRLayerDecoratorTest, NullDecoratedLayerGuards) {
  OGRMemLayer dummy{"dummy", nullptr, wkbPoint};
  NullDecoratedLayer null_decorator(&dummy);

  EXPECT_EQ(nullptr, null_decorator.GetSpatialFilter());
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.ISetSpatialFilter(0, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.SetAttributeFilter("1=1"));
  null_decorator.ResetReading();
  EXPECT_EQ(nullptr, null_decorator.GetNextFeature());
  EXPECT_EQ(nullptr, null_decorator.GetDataset());

  ArrowArrayStream stream;
  EXPECT_FALSE(null_decorator.GetArrowStream(&stream, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.SetNextByIndex(0));
  EXPECT_EQ(nullptr, null_decorator.GetFeature(0));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.ISetFeature(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.ICreateFeature(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.IUpsertFeature(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.DeleteFeature(0));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.IUpdateFeature(nullptr, 0, nullptr,
                                                          0, nullptr, false));

  EXPECT_STREQ("dummy", null_decorator.GetName());
  EXPECT_EQ(wkbNone, null_decorator.GetGeomType());
  EXPECT_EQ(nullptr, null_decorator.GetLayerDefn());
  EXPECT_EQ(nullptr, null_decorator.GetSpatialRef());

  EXPECT_EQ(0, null_decorator.GetFeatureCount());
  OGREnvelope sEnvelope;
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.IGetExtent(0, &sEnvelope));
  OGREnvelope3D sEnvelope3D;
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.IGetExtent3D(0, &sEnvelope3D));

  EXPECT_FALSE(null_decorator.TestCapability(OLCRandomRead));
  OGRFieldDefn oField("f", OFTInteger);
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.CreateField(&oField));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.DeleteField(0));
  int anMap[] = {0};
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.ReorderFields(anMap));
  EXPECT_EQ(OGRERR_FAILURE,
            null_decorator.AlterFieldDefn(0, &oField, ALTER_ALL_FLAG));
  OGRGeomFieldDefn oGeomField("geom", wkbPoint);
  EXPECT_EQ(OGRERR_FAILURE,
            null_decorator.AlterGeomFieldDefn(0, &oGeomField, ALTER_ALL_FLAG));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.CreateGeomField(&oGeomField));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.SyncToDisk());

  EXPECT_EQ(nullptr, null_decorator.GetStyleTable());
  null_decorator.SetStyleTableDirectly(nullptr);
  null_decorator.SetStyleTable(nullptr);

  EXPECT_EQ(OGRERR_FAILURE, null_decorator.StartTransaction());
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.CommitTransaction());
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.RollbackTransaction());

  EXPECT_STREQ("", null_decorator.GetFIDColumn());
  EXPECT_STREQ("", null_decorator.GetGeometryColumn());
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.SetIgnoredFields(nullptr));

  EXPECT_EQ(nullptr, null_decorator.GetMetadataDomainList());
  EXPECT_EQ(nullptr, null_decorator.GetMetadata());
  EXPECT_EQ(CE_Failure, null_decorator.SetMetadata(nullptr));
  EXPECT_EQ(nullptr, null_decorator.GetMetadataItem("item"));
  EXPECT_EQ(CE_Failure, null_decorator.SetMetadataItem("item", "val"));
  EXPECT_EQ(OGRERR_FAILURE, null_decorator.Rename("new_name"));
}

TEST(OGRLayerDecoratorTest, ForwardingCalls) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  auto mem_layer = std::make_unique<OGRMemLayer>("mem_layer", &srs, wkbPoint);

  OGRFieldDefn field_int("int_fld", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, mem_layer->CreateField(&field_int));

  OGRFieldDefn field_str("str_fld", OFTString);
  ASSERT_EQ(OGRERR_NONE, mem_layer->CreateField(&field_str));

  // Add a feature
  {
    auto feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    feat->SetField(0, 123);
    feat->SetField(1, "abc");
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(feat.get()));
  }

  // Wrap in OGRLayerDecorator
  OGRLayerDecorator decorator(mem_layer.get(), /*bTakeOwnership=*/false);
  EXPECT_EQ(mem_layer.get(), decorator.GetBaseLayer());
  EXPECT_STREQ("mem_layer", decorator.GetName());
  EXPECT_EQ(wkbPoint, decorator.GetGeomType());
  EXPECT_NE(nullptr, decorator.GetLayerDefn());
  EXPECT_NE(nullptr, decorator.GetSpatialRef());
  EXPECT_EQ(1, decorator.GetFeatureCount());

  // Filter methods
  OGRPoint filter_pt(10.0, 20.0);
  EXPECT_EQ(OGRERR_NONE, decorator.ISetSpatialFilter(0, &filter_pt));
  EXPECT_NE(nullptr, decorator.GetSpatialFilter());
  EXPECT_EQ(OGRERR_NONE, decorator.ISetSpatialFilter(0, nullptr));
  EXPECT_EQ(nullptr, decorator.GetSpatialFilter());
  EXPECT_EQ(OGRERR_NONE, decorator.SetAttributeFilter("int_fld = 123"));
  EXPECT_EQ(OGRERR_NONE, decorator.SetAttributeFilter(nullptr));

  // Feature access
  decorator.ResetReading();
  OGRFeature* read_feat = decorator.GetNextFeature();
  ASSERT_NE(nullptr, read_feat);
  EXPECT_EQ(123, read_feat->GetFieldAsInteger(0));
  OGRFeature::DestroyFeature(read_feat);

  EXPECT_EQ(OGRERR_NONE, decorator.SetNextByIndex(0));
  OGRFeature* by_fid = decorator.GetFeature(0);
  ASSERT_NE(nullptr, by_fid);
  by_fid->SetField(0, 456);
  EXPECT_EQ(OGRERR_NONE, decorator.ISetFeature(by_fid));
  OGRFeature::DestroyFeature(by_fid);

  // Field manipulation
  int panMap[] = {1, 0};
  EXPECT_EQ(OGRERR_NONE, decorator.ReorderFields(panMap));

  OGRFieldDefn field_new("new_fld", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, decorator.CreateField(&field_new));
  EXPECT_EQ(OGRERR_NONE, decorator.DeleteField(2));

  // Extent
  OGREnvelope envelope;
  EXPECT_EQ(OGRERR_NONE, decorator.IGetExtent(0, &envelope));
  EXPECT_DOUBLE_EQ(10.0, envelope.MinX);
  EXPECT_DOUBLE_EQ(10.0, envelope.MaxX);

  OGREnvelope3D envelope3D;
  EXPECT_EQ(OGRERR_NONE, decorator.IGetExtent3D(0, &envelope3D));

  // ArrowStream
  ArrowArrayStream stream;
  EXPECT_TRUE(decorator.GetArrowStream(&stream, nullptr));
  if (stream.release) {
    stream.release(&stream);
  }

  // Capability & Transactions
  EXPECT_TRUE(decorator.TestCapability(OLCRandomRead));
  EXPECT_EQ(OGRERR_NONE, decorator.SyncToDisk());
  EXPECT_EQ(OGRERR_NONE, decorator.StartTransaction());
  EXPECT_EQ(OGRERR_NONE, decorator.CommitTransaction());
  EXPECT_EQ(OGRERR_NONE, decorator.StartTransaction());
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, decorator.RollbackTransaction());

  // StyleTable
  EXPECT_EQ(nullptr, decorator.GetStyleTable());
  decorator.SetStyleTableDirectly(new OGRStyleTable());
  EXPECT_NE(nullptr, decorator.GetStyleTable());
  OGRStyleTable tbl2;
  decorator.SetStyleTable(&tbl2);

  // Additional forwarding calls
  EXPECT_EQ(mem_layer->GetDataset(), decorator.GetDataset());
  EXPECT_EQ(1, decorator.FindFieldIndex("int_fld", true));
  EXPECT_TRUE(decorator.GetSupportedSRSList(0).empty());
  EXPECT_EQ(OGRERR_FAILURE, decorator.SetActiveSRS(0, nullptr));

  {
    auto feat_create = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    feat_create->SetFID(10);
    feat_create->SetField(0, 999);
    EXPECT_EQ(OGRERR_NONE, decorator.ICreateFeature(feat_create.get()));
    EXPECT_EQ(OGRERR_NONE, decorator.IUpsertFeature(feat_create.get()));
    int updated_idx = 0;
    EXPECT_EQ(OGRERR_NONE,
              decorator.IUpdateFeature(feat_create.get(), 1, &updated_idx, 0,
                                       nullptr, false));
  }

  OGRGeomFieldDefn geom_fld_new("geom2", wkbPoint);
  EXPECT_EQ(OGRERR_NONE, decorator.CreateGeomField(&geom_fld_new));
  EXPECT_EQ(OGRERR_NONE,
            decorator.AlterFieldDefn(0, &field_int, ALTER_NAME_FLAG));
  EXPECT_EQ(OGRERR_NONE,
            decorator.AlterGeomFieldDefn(0, &geom_fld_new, ALTER_NAME_FLAG));

  EXPECT_STREQ("", decorator.GetFIDColumn());
  EXPECT_STREQ("", decorator.GetGeometryColumn());
  const char* ignored_flds[] = {"int_fld", nullptr};
  EXPECT_EQ(OGRERR_NONE, decorator.SetIgnoredFields(ignored_flds));
  EXPECT_EQ(OGRERR_NONE, decorator.SetIgnoredFields(nullptr));

  EXPECT_EQ(nullptr, decorator.GetMetadataDomainList());
  EXPECT_EQ(nullptr, decorator.GetMetadata());
  char* meta[] = {nullptr};
  EXPECT_EQ(CE_None, decorator.SetMetadata(meta));
  EXPECT_EQ(nullptr, decorator.GetMetadataItem("foo"));
  EXPECT_EQ(CE_None, decorator.SetMetadataItem("foo", "bar"));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, decorator.Rename("renamed_layer"));

  // DeleteFeature
  EXPECT_EQ(OGRERR_NONE, decorator.DeleteFeature(0));
  EXPECT_EQ(OGRERR_NONE, decorator.DeleteFeature(10));
}

TEST(OGRLayerDecoratorTest, OwnershipTransfer) {
  auto* mem_layer = new OGRMemLayer("owned_layer", nullptr, wkbNone);
  {
    OGRLayerDecorator decorator(mem_layer, /*bTakeOwnership=*/true);
    EXPECT_STREQ("owned_layer", decorator.GetName());
  }  // destructor deletes mem_layer
}

// TODO: b/335317901 - Cover remaining error branches in ogrlayerdecorator.cpp.

}  // namespace
}  // namespace autotest2
