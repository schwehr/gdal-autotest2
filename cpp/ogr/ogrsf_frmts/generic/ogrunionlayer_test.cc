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

// Tests for OGRUnionLayer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrunionlayer.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrunionlayer.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrunionlayer.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

OGRLayer** AllocateLayerArray(const std::vector<OGRLayer*>& layers) {
  auto papo =
      static_cast<OGRLayer**>(CPLMalloc(sizeof(OGRLayer*) * layers.size()));
  for (size_t i = 0; i < layers.size(); ++i) {
    papo[i] = layers[i];
  }
  return papo;
}

class IgnorableMemLayer : public OGRMemLayer {
 public:
  IgnorableMemLayer(const char* pszName, const OGRSpatialReference* poSRSIn,
                    OGRwkbGeometryType eType)
      : OGRMemLayer(pszName, poSRSIn, eType) {}

  int TestCapability(const char* pszCap) const override {
    if (EQUAL(pszCap, OLCIgnoreFields)) return TRUE;
    return OGRMemLayer::TestCapability(pszCap);
  }

  OGRErr SyncToDisk() override { return OGRERR_NONE; }
};

TEST(OGRUnionLayerTest, GeomFieldDefnConstructors) {
  OGRUnionLayerGeomFieldDefn defn1("g1", wkbPoint);
  EXPECT_STREQ("g1", defn1.GetNameRef());
  EXPECT_EQ(wkbPoint, defn1.GetType());

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  OGRGeomFieldDefn src_fld("g_src", wkbPolygon);
  src_fld.SetSpatialRef(&srs);
  OGRUnionLayerGeomFieldDefn defn2(&src_fld);
  EXPECT_STREQ("g_src", defn2.GetNameRef());
  EXPECT_EQ(wkbPolygon, defn2.GetType());
  EXPECT_NE(nullptr, defn2.GetSpatialRef());

  defn2.bGeomTypeSet = true;
  defn2.bSRSSet = true;
  defn2.sStaticEnvelope.MinX = 1.0;
  defn2.sStaticEnvelope.MaxX = 2.0;
  OGRUnionLayerGeomFieldDefn defn3(&defn2);
  EXPECT_STREQ("g_src", defn3.GetNameRef());
  EXPECT_TRUE(defn3.bGeomTypeSet);
  EXPECT_TRUE(defn3.bSRSSet);
  EXPECT_DOUBLE_EQ(1.0, defn3.sStaticEnvelope.MinX);

  OSRCleanup();
}

TEST(OGRUnionLayerTest, FieldUnionAllLayersAndAutoWarp) {
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto l1 = std::make_unique<IgnorableMemLayer>("layer1", &srs4326, wkbPoint);
  auto l2 = std::make_unique<IgnorableMemLayer>("layer2", &srs3857, wkbPoint);
  auto l3 = std::make_unique<IgnorableMemLayer>("layer3", nullptr, wkbPoint);

  OGRFieldDefn f_ir1("f_int_real", OFTInteger);
  f_ir1.SetWidth(10);
  OGRFieldDefn f_ri1("f_real_int", OFTReal);
  f_ri1.SetWidth(12);
  f_ri1.SetPrecision(2);
  OGRFieldDefn f_ii64_1("f_int_int64", OFTInteger);
  OGRFieldDefn f_i64i_1("f_int64_int", OFTInteger64);
  OGRFieldDefn f_is1("f_int_str", OFTInteger);
  OGRFieldDefn f_only1("f_only_l1", OFTString);

  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_ir1));
  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_ri1));
  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_ii64_1));
  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_i64i_1));
  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_is1));
  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_only1));

  {
    auto feat = std::make_unique<OGRFeature>(l1->GetLayerDefn());
    feat->SetFID(10);
    feat->SetField("f_int_real", 1);
    feat->SetField("f_real_int", 1.5);
    feat->SetField("f_int_int64", 2);
    feat->SetField("f_int64_int", static_cast<GIntBig>(1000));
    feat->SetField("f_int_str", 42);
    feat->SetField("f_only_l1", "first");
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, l1->CreateFeature(feat.get()));
  }
  {
    auto feat = std::make_unique<OGRFeature>(l1->GetLayerDefn());
    feat->SetFID(11);
    feat->SetField("f_int_real", 2);
    feat->SetField("f_real_int", 2.5);
    auto pt = std::make_unique<OGRPoint>(11.0, 21.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, l1->CreateFeature(feat.get()));
  }

  OGRFieldDefn f_ir2("f_int_real", OFTReal);
  f_ir2.SetWidth(8);
  f_ir2.SetPrecision(2);
  OGRFieldDefn f_ri2("f_real_int", OFTInteger);
  OGRFieldDefn f_ii64_2("f_int_int64", OFTInteger64);
  OGRFieldDefn f_i64i_2("f_int64_int", OFTInteger);
  OGRFieldDefn f_is2("f_int_str", OFTString);
  OGRFieldDefn f_only2("f_only_l2", OFTString);

  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f_ir2));
  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f_ri2));
  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f_ii64_2));
  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f_i64i_2));
  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f_is2));
  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f_only2));

  {
    auto feat = std::make_unique<OGRFeature>(l2->GetLayerDefn());
    feat->SetFID(20);
    feat->SetField("f_int_real", 3.14);
    feat->SetField("f_real_int", 5);
    feat->SetField("f_int_int64", static_cast<GIntBig>(5000));
    feat->SetField("f_int64_int", 6);
    feat->SetField("f_int_str", "string_val");
    feat->SetField("f_only_l2", "second");
    auto pt = std::make_unique<OGRPoint>(111319.49, 111888.99);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, l2->CreateFeature(feat.get()));
  }

  ASSERT_EQ(OGRERR_NONE, l3->CreateField(&f_only2));
  {
    auto feat = std::make_unique<OGRFeature>(l3->GetLayerDefn());
    feat->SetFID(30);
    feat->SetField("f_only_l2", "third");
    ASSERT_EQ(OGRERR_NONE, l3->CreateFeature(feat.get()));
  }

  std::vector<OGRLayer*> src_layers = {l1.get(), l2.get(), l3.get()};
  OGRUnionLayer union_layer("union_all", static_cast<int>(src_layers.size()),
                            AllocateLayerArray(src_layers),
                            /*bTakeLayerOwnership=*/false);

  union_layer.SetPreserveSrcFID(true);

  EXPECT_STREQ("union_all", union_layer.GetName());
  EXPECT_EQ(wkbPoint, union_layer.GetGeomType());
  EXPECT_NE(nullptr, union_layer.GetSpatialRef());

  const OGRFeatureDefn* defn = union_layer.GetLayerDefn();
  ASSERT_NE(nullptr, defn);
  // Merged field types per ogrunionlayer.cpp logic
  EXPECT_EQ(OFTString,
            defn->GetFieldDefn(defn->GetFieldIndex("f_int_real"))->GetType());
  EXPECT_EQ(0,
            defn->GetFieldDefn(defn->GetFieldIndex("f_int_real"))->GetWidth());
  EXPECT_EQ(OFTReal,
            defn->GetFieldDefn(defn->GetFieldIndex("f_real_int"))->GetType());
  EXPECT_EQ(OFTInteger64,
            defn->GetFieldDefn(defn->GetFieldIndex("f_int_int64"))->GetType());
  EXPECT_EQ(OFTInteger64,
            defn->GetFieldDefn(defn->GetFieldIndex("f_int64_int"))->GetType());
  EXPECT_EQ(OFTString,
            defn->GetFieldDefn(defn->GetFieldIndex("f_int_str"))->GetType());

  // Test SetIgnoredFields and feature iteration across all 3 layers
  const char* ignored[] = {"f_only_l2", "OGR_GEOMETRY", "OGR_STYLE", nullptr};
  EXPECT_EQ(OGRERR_NONE, union_layer.SetIgnoredFields(ignored));

  int count = 0;
  {
    WithQuietHandler quiet_handler;
    for (auto&& feat : union_layer) {
      count++;
      EXPECT_NE(nullptr, feat);
    }
  }
  EXPECT_EQ(4, count);

  EXPECT_EQ(OGRERR_NONE, union_layer.SetIgnoredFields(nullptr));

  // Test GetFeature with spatial filter active (exercises save & restore in
  // GetFeature)
  OGRPoint filter_pt(10.0, 20.0);
  EXPECT_EQ(OGRERR_NONE, union_layer.ISetSpatialFilter(0, &filter_pt));
  {
    WithQuietHandler quiet_handler;
    std::unique_ptr<OGRFeature> f10(union_layer.GetFeature(10));
    ASSERT_NE(nullptr, f10);
    EXPECT_EQ(10, f10->GetFID());

    std::unique_ptr<OGRFeature> f20(union_layer.GetFeature(20));
    ASSERT_NE(nullptr, f20);
    EXPECT_EQ(20, f20->GetFID());

    EXPECT_EQ(nullptr, union_layer.GetFeature(9999));
  }
  EXPECT_EQ(OGRERR_NONE, union_layer.ISetSpatialFilter(0, nullptr));
  EXPECT_EQ(OGRERR_NONE, union_layer.ISetSpatialFilter(99, nullptr));

  // Test SetAttributeFilter
  EXPECT_EQ(OGRERR_NONE,
            union_layer.SetAttributeFilter("OGR_GEOMETRY = 'POINT'"));
  EXPECT_EQ(OGRERR_NONE,
            union_layer.SetAttributeFilter("OGR_GEOMETRY = 'POINT'"));
  EXPECT_EQ(OGRERR_NONE, union_layer.SetAttributeFilter(nullptr));
  EXPECT_EQ(OGRERR_NONE, union_layer.SetAttributeFilter(nullptr));

  // Non-pass-through filter
  EXPECT_EQ(OGRERR_NONE, union_layer.SetAttributeFilter("f_only_l1 = 'first'"));
  EXPECT_FALSE(union_layer.TestCapability(OLCFastFeatureCount));
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(1, union_layer.GetFeatureCount(true));
  }
  EXPECT_EQ(OGRERR_NONE, union_layer.SetAttributeFilter(nullptr));

  // Extents
  OGREnvelope env;
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_NONE, union_layer.IGetExtent(0, &env, true));
    EXPECT_EQ(OGRERR_FAILURE, union_layer.IGetExtent(99, &env, true));
    EXPECT_EQ(OGRERR_FAILURE, union_layer.IGetExtent(-1, &env, true));
  }

  // Capabilities
  EXPECT_TRUE(union_layer.TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(union_layer.TestCapability(OLCFastGetExtent));
  EXPECT_FALSE(union_layer.TestCapability(OLCFastSpatialFilter));
  EXPECT_FALSE(union_layer.TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(union_layer.TestCapability(OLCRandomRead));
  EXPECT_FALSE(union_layer.TestCapability(OLCRandomWrite));
  EXPECT_FALSE(union_layer.TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(union_layer.TestCapability(OLCIgnoreFields));
  EXPECT_TRUE(union_layer.TestCapability(OLCCurveGeometries));
  EXPECT_FALSE(union_layer.TestCapability("UnknownCapability"));

  OSRCleanup();
}

TEST(OGRUnionLayerTest, FieldStrategiesAndStaticMetadata) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  // Strategy 1: FIELD_FROM_FIRST_LAYER with nGeomFields = 0 and nGeomFields =
  // -1
  {
    auto l1 = std::make_unique<OGRMemLayer>("l1", &srs, wkbPoint);
    OGRFieldDefn f1("f1", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f1));

    std::vector<OGRLayer*> layers = {l1.get()};
    OGRUnionLayer ul("ul_first", 1, AllocateLayerArray(layers), false);
    ul.SetFields(FIELD_FROM_FIRST_LAYER, 0, nullptr, 0, nullptr);
    EXPECT_EQ(wkbPoint, ul.GetGeomType());
    EXPECT_EQ(1, ul.GetLayerDefn()->GetFieldCount());
  }
  {
    auto l1 = std::make_unique<OGRMemLayer>("l1", &srs, wkbPoint);
    OGRFieldDefn f1("f1", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f1));

    std::vector<OGRLayer*> layers = {l1.get()};
    OGRUnionLayer ul("ul_no_geom", 1, AllocateLayerArray(layers), false);
    ul.SetFields(FIELD_FROM_FIRST_LAYER, 0, nullptr, -1, nullptr);
    EXPECT_EQ(wkbNone, ul.GetGeomType());
    EXPECT_EQ(nullptr, ul.GetSpatialRef());
    EXPECT_EQ(0, ul.GetLayerDefn()->GetGeomFieldCount());
  }

  // Strategy 2: FIELD_INTERSECTION_ALL_LAYERS
  {
    auto l1 = std::make_unique<OGRMemLayer>("l1", &srs, wkbPoint);
    auto l2 = std::make_unique<OGRMemLayer>("l2", &srs, wkbPoint);

    OGRFieldDefn common_fld("common", OFTInteger);
    OGRFieldDefn l1_fld("only_l1", OFTString);
    OGRFieldDefn l2_fld("only_l2", OFTReal);

    ASSERT_EQ(OGRERR_NONE, l1->CreateField(&common_fld));
    ASSERT_EQ(OGRERR_NONE, l1->CreateField(&l1_fld));
    ASSERT_EQ(OGRERR_NONE, l2->CreateField(&common_fld));
    ASSERT_EQ(OGRERR_NONE, l2->CreateField(&l2_fld));

    std::vector<OGRLayer*> layers = {l1.get(), l2.get()};
    OGRUnionLayer ul("ul_intersect", 2, AllocateLayerArray(layers), false);
    ul.SetSourceLayerFieldName("src");
    ul.SetFields(FIELD_INTERSECTION_ALL_LAYERS, 0, nullptr, 0, nullptr);

    const OGRFeatureDefn* defn = ul.GetLayerDefn();
    EXPECT_EQ(2, defn->GetFieldCount());  // src + common
    EXPECT_EQ(0, defn->GetFieldIndex("src"));
    EXPECT_EQ(1, defn->GetFieldIndex("common"));
    EXPECT_EQ(-1, defn->GetFieldIndex("only_l1"));
    EXPECT_EQ(-1, defn->GetFieldIndex("only_l2"));
  }

  // Strategy 3: FIELD_SPECIFIED with static envelope and feature count
  {
    auto l1 = std::make_unique<OGRMemLayer>("l1", nullptr, wkbNone);
    OGRGeomFieldDefn gf_l1("geom", wkbPoint);
    gf_l1.SetSpatialRef(&srs);
    ASSERT_EQ(OGRERR_NONE, l1->CreateGeomField(&gf_l1));
    OGRFieldDefn f_src("f_spec", OFTString);
    ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f_src));

    std::vector<OGRLayer*> layers = {l1.get()};
    OGRUnionLayer ul("ul_spec", 1, AllocateLayerArray(layers), false);

    OGRFieldDefn f_spec("f_spec", OFTString);
    OGRFieldDefn* pFields[] = {&f_spec};

    OGRUnionLayerGeomFieldDefn gf_spec("geom", wkbUnknown);
    gf_spec.bGeomTypeSet = false;
    gf_spec.bSRSSet = false;
    gf_spec.sStaticEnvelope.MinX = 0.0;
    gf_spec.sStaticEnvelope.MaxX = 10.0;
    gf_spec.sStaticEnvelope.MinY = 0.0;
    gf_spec.sStaticEnvelope.MaxY = 20.0;
    OGRUnionLayerGeomFieldDefn* pGeomFields[] = {&gf_spec};

    ul.SetFields(FIELD_SPECIFIED, 1, pFields, 1, pGeomFields);
    ul.SetFeatureCount(50);

    EXPECT_TRUE(ul.TestCapability(OLCFastFeatureCount));
    EXPECT_EQ(50, ul.GetFeatureCount(true));
    EXPECT_TRUE(ul.TestCapability(OLCFastGetExtent));

    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, ul.IGetExtent(0, &env, false));
    EXPECT_DOUBLE_EQ(0.0, env.MinX);
    EXPECT_DOUBLE_EQ(10.0, env.MaxX);

    const OGRFeatureDefn* defn = ul.GetLayerDefn();
    ASSERT_NE(nullptr, defn);
    EXPECT_EQ(wkbPoint, defn->GetGeomFieldDefn(0)->GetType());
    EXPECT_NE(nullptr, defn->GetGeomFieldDefn(0)->GetSpatialRef());
  }

  // Strategy 4: FIELD_UNION_ALL_LAYERS with nGeomFields == 1
  {
    auto l1 = std::make_unique<OGRMemLayer>("l1", nullptr, wkbNone);
    OGRGeomFieldDefn gf_l1("geom", wkbPoint);
    ASSERT_EQ(OGRERR_NONE, l1->CreateGeomField(&gf_l1));
    std::vector<OGRLayer*> layers = {l1.get()};
    OGRUnionLayer ul("ul_union_g1", 1, AllocateLayerArray(layers), false);

    OGRUnionLayerGeomFieldDefn gf("geom", wkbPoint);
    gf.bGeomTypeSet = false;
    gf.bSRSSet = false;
    OGRUnionLayerGeomFieldDefn* pGeomFields[] = {&gf};

    ul.SetFields(FIELD_UNION_ALL_LAYERS, 0, nullptr, 1, pGeomFields);
    const OGRFeatureDefn* defn = ul.GetLayerDefn();
    ASSERT_NE(nullptr, defn);
    EXPECT_EQ(1, defn->GetGeomFieldCount());
  }

  OSRCleanup();
}

TEST(OGRUnionLayerTest, WriteOperationsAndErrorPaths) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  // Error paths without SourceLayerFieldName or PreserveSrcFID
  {
    auto l1 = std::make_unique<IgnorableMemLayer>("l1", &srs, wkbPoint);
    std::vector<OGRLayer*> layers = {l1.get()};
    OGRUnionLayer ul("ul_err", 1, AllocateLayerArray(layers), false);

    OGRFeature feat(ul.GetLayerDefn());
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, ul.ICreateFeature(&feat));
    EXPECT_EQ(OGRERR_FAILURE, ul.ISetFeature(&feat));
    int fld_idx = 0;
    EXPECT_EQ(OGRERR_FAILURE,
              ul.IUpdateFeature(&feat, 0, &fld_idx, 0, nullptr, false));
    EXPECT_EQ(nullptr, ul.GetFeature(0));
  }

  // Writable union layer over 2 layers
  auto l1 = std::make_unique<IgnorableMemLayer>("l1", &srs, wkbPoint);
  auto l2 = std::make_unique<IgnorableMemLayer>("l2", &srs, wkbPoint);

  OGRGeomFieldDefn gf2("geom2", wkbPoint);
  ASSERT_EQ(OGRERR_NONE, l1->CreateGeomField(&gf2));
  ASSERT_EQ(OGRERR_NONE, l2->CreateGeomField(&gf2));

  OGRFieldDefn f1("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, l1->CreateField(&f1));
  ASSERT_EQ(OGRERR_NONE, l2->CreateField(&f1));

  std::vector<OGRLayer*> layers = {l1.get(), l2.get()};
  OGRUnionLayer ul("ul_rw", 2, AllocateLayerArray(layers), false);
  ul.SetSourceLayerFieldName("src_layer");
  ul.SetPreserveSrcFID(true);

  // Test error conditions with quiet handler
  {
    WithQuietHandler quiet_handler;
    OGRFeature f_bad(ul.GetLayerDefn());

    // CreateFeature with FID set
    f_bad.SetFID(10);
    EXPECT_EQ(OGRERR_FAILURE, ul.ICreateFeature(&f_bad));
    f_bad.SetFID(OGRNullFID);

    // CreateFeature without src_layer field set
    EXPECT_EQ(OGRERR_FAILURE, ul.ICreateFeature(&f_bad));

    // CreateFeature with non-existent src_layer
    f_bad.SetField(0, "non_existent");
    EXPECT_EQ(OGRERR_FAILURE, ul.ICreateFeature(&f_bad));

    // SetFeature with FID not set
    f_bad.SetFID(OGRNullFID);
    EXPECT_EQ(OGRERR_FAILURE, ul.ISetFeature(&f_bad));

    // SetFeature with src_layer not set
    f_bad.SetFID(1);
    f_bad.SetFieldNull(0);
    EXPECT_EQ(OGRERR_FAILURE, ul.ISetFeature(&f_bad));

    // SetFeature with non-existent src_layer
    f_bad.SetField(0, "non_existent");
    EXPECT_EQ(OGRERR_FAILURE, ul.ISetFeature(&f_bad));

    // UpdateFeature with FID not set
    f_bad.SetFID(OGRNullFID);
    EXPECT_EQ(OGRERR_FAILURE,
              ul.IUpdateFeature(&f_bad, 0, nullptr, 0, nullptr, false));

    // UpdateFeature with src_layer not set
    f_bad.SetFID(1);
    f_bad.SetFieldNull(0);
    EXPECT_EQ(OGRERR_FAILURE,
              ul.IUpdateFeature(&f_bad, 0, nullptr, 0, nullptr, false));

    // UpdateFeature with non-existent src_layer
    f_bad.SetField(0, "non_existent");
    EXPECT_EQ(OGRERR_FAILURE,
              ul.IUpdateFeature(&f_bad, 0, nullptr, 0, nullptr, false));
  }

  // Successful CreateFeature on l1
  {
    OGRFeature f_create(ul.GetLayerDefn());
    f_create.SetField("src_layer", "l1");
    f_create.SetField("val", 100);
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    f_create.SetGeomFieldDirectly(0, pt.release());
    EXPECT_EQ(OGRERR_NONE, ul.ICreateFeature(&f_create));
    GIntBig created_fid = f_create.GetFID();
    EXPECT_GE(created_fid, 0);

    // Successful SetFeature
    f_create.SetField("val", 200);
    EXPECT_EQ(OGRERR_NONE, ul.ISetFeature(&f_create));

    // Successful IUpdateFeature with field indices {0, 1} and geom indices {0,
    // 1}
    int updated_fields[] = {0, 1};
    int updated_geoms[] = {0, 1};
    EXPECT_EQ(OGRERR_NONE, ul.IUpdateFeature(&f_create, 2, updated_fields, 2,
                                             updated_geoms, false));
  }

  // Successful IUpsertFeature on new feature (calls ICreateFeature)
  {
    OGRFeature f_new(ul.GetLayerDefn());
    f_new.SetFID(OGRNullFID);
    f_new.SetField("src_layer", "l2");
    f_new.SetField("val", 500);
    EXPECT_EQ(OGRERR_NONE, ul.IUpsertFeature(&f_new));
  }

  // Sync to disk
  EXPECT_EQ(OGRERR_NONE, ul.SyncToDisk());

  OSRCleanup();
}

// TODO: b/335317901 - Cover remaining error branches in ogrunionlayer.cpp.

}  // namespace
}  // namespace autotest2
