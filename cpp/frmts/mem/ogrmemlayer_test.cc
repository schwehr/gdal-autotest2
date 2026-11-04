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
// Tests OGRMemLayer in-memory vector layer implementation (ogrmemlayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/mem/ogrmemlayer.cpp
//   https://gdal.org/drivers/vector/mem.html

#include "third_party/gdal/frmts/mem/memdataset.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/ogr/ogrfeaturedefn.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::IsNull;
using ::testing::NotNull;

// Helper to create an OGRSpatialReference for WGS84.
OGRSpatialReference GetWgs84Srs() {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  return srs;
}

// -----------------------------------------------------------------------------
// 1. Constructors and Accessors
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, ConstructorsAndAccessors) {
  const OGRSpatialReference wgs84 = GetWgs84Srs();

  // 1. Construct with name, SRS, and geometry type.
  {
    OGRMemLayer layer("pts", &wgs84, wkbPoint);
    EXPECT_STREQ("pts", layer.GetName());
    EXPECT_STREQ("pts", layer.GetDescription());
    ASSERT_THAT(layer.GetLayerDefn(), NotNull());
    EXPECT_EQ(wkbPoint, layer.GetLayerDefn()->GetGeomType());
    ASSERT_EQ(1, layer.GetLayerDefn()->GetGeomFieldCount());
    const OGRSpatialReference* srs =
        layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef();
    ASSERT_THAT(srs, NotNull());
    EXPECT_THAT(*srs, IsSameAs(wgs84));
  }

  // 2. Construct with wkbNone and null SRS.
  {
    OGRMemLayer layer("nogeom", nullptr, wkbNone);
    EXPECT_STREQ("nogeom", layer.GetName());
    ASSERT_THAT(layer.GetLayerDefn(), NotNull());
    EXPECT_EQ(wkbNone, layer.GetLayerDefn()->GetGeomType());
  }

  // 3. Construct with OGRFeatureDefn.
  {
    OGRFeatureDefnBuilder builder("custom_defn");
    builder.AddGeom("geom", wkbLineString);
    builder.AddField("int_fld", OFTInteger, OFSTNone);
    OGRFeatureDefn* fd = builder.Build();
    ASSERT_THAT(fd, NotNull());
    absl::Cleanup fd_cleanup = [fd] { fd->Release(); };

    OGRMemLayer layer(*fd);
    EXPECT_STREQ("custom_defn", layer.GetName());
    ASSERT_THAT(layer.GetLayerDefn(), NotNull());
    EXPECT_EQ(wkbLineString, layer.GetLayerDefn()->GetGeomType());
    EXPECT_EQ(1, layer.GetLayerDefn()->GetFieldCount());
  }

  // 4. Accessors and flags.
  {
    OGRMemLayer layer("test_acc", nullptr, wkbPoint);
    EXPECT_STREQ("", layer.GetFIDColumn());
    layer.SetFIDColumn("custom_fid");
    EXPECT_STREQ("custom_fid", layer.GetFIDColumn());

    EXPECT_TRUE(layer.IsUpdatable());
    layer.SetUpdatable(false);
    EXPECT_FALSE(layer.IsUpdatable());
    layer.SetUpdatable(true);
    EXPECT_TRUE(layer.IsUpdatable());

    layer.SetAdvertizeUTF8(true);
    EXPECT_EQ(TRUE, layer.TestCapability(OLCStringsAsUTF8));

    EXPECT_FALSE(layer.HasBeenUpdated());
    layer.SetUpdated(true);
    EXPECT_TRUE(layer.HasBeenUpdated());
    layer.SetUpdated(false);
    EXPECT_FALSE(layer.HasBeenUpdated());

    EXPECT_EQ(0, layer.GetNextReadFID());

    MEMDataset ds;
    layer.SetDataset(&ds);
    EXPECT_EQ(&ds, layer.GetDataset());
  }
}

// -----------------------------------------------------------------------------
// 2. TestCapability
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, TestCapabilityAllBranches) {
  OGRMemLayer layer("cap_layer", nullptr, wkbPoint);

  // When updatable:
  EXPECT_EQ(TRUE, layer.TestCapability(OLCRandomRead));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCSequentialWrite));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCRandomWrite));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCFastSpatialFilter));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCDeleteFeature));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCUpsertFeature));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCUpdateFeature));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCCreateField));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCCreateGeomField));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCDeleteField));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCReorderFields));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCAlterFieldDefn));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCAlterGeomFieldDefn));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCFastSetNextByIndex));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCStringsAsUTF8));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCCurveGeometries));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCMeasuredGeometries));
  EXPECT_EQ(TRUE, layer.TestCapability(OLCZGeometries));
  EXPECT_EQ(FALSE, layer.TestCapability("UnknownCapability"));

  // When not updatable:
  layer.SetUpdatable(false);
  EXPECT_EQ(FALSE, layer.TestCapability(OLCSequentialWrite));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCRandomWrite));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCDeleteFeature));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCUpsertFeature));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCUpdateFeature));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCCreateField));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCCreateGeomField));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCDeleteField));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCReorderFields));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCAlterFieldDefn));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCAlterGeomFieldDefn));

  // OLCFastFeatureCount with spatial or attribute filter.
  layer.SetUpdatable(true);
  layer.SetSpatialFilterRect(0, 0, 10, 10);
  EXPECT_EQ(FALSE, layer.TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCFastSetNextByIndex));
  layer.SetSpatialFilter(nullptr);

  layer.SetAttributeFilter("1 = 1");
  EXPECT_EQ(FALSE, layer.TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(FALSE, layer.TestCapability(OLCFastSetNextByIndex));
  layer.SetAttributeFilter(nullptr);
}

// -----------------------------------------------------------------------------
// 3. Array Mode Create, Set, Get, Delete, and Iteration
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, ArrayModeCreateSetGetDeleteAndIteration) {
  const OGRSpatialReference wgs84 = GetWgs84Srs();
  auto layer = std::make_unique<OGRMemLayer>("arr_layer", &wgs84, wkbPoint);

  OGRFieldDefn fld("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&fld));

  // 1. Error paths for ISetFeature: nullptr feature and negative FID < -1.
  EXPECT_EQ(OGRERR_FAILURE, layer->ISetFeature(nullptr));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    OGRFeature bad_fid_feat(layer->GetLayerDefn());
    bad_fid_feat.SetFID(-2);
    EXPECT_EQ(OGRERR_FAILURE, layer->ISetFeature(&bad_fid_feat));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. ISetFeature on a fresh layer with FID 0, then ISetFeature with OGRNullFID
  // so next create FID loop advances.
  {
    OGRFeature feat0(layer->GetLayerDefn());
    feat0.SetFID(0);
    feat0.SetField("val", 100);
    EXPECT_EQ(OGRERR_NONE, layer->ISetFeature(&feat0));

    OGRFeature feat_null(layer->GetLayerDefn());
    feat_null.SetFID(OGRNullFID);
    feat_null.SetField("val", 101);
    EXPECT_EQ(OGRERR_NONE, layer->ISetFeature(&feat_null));
    EXPECT_EQ(1, feat_null.GetFID());
  }

  // 3. Overwrite existing FID via ISetFeature.
  {
    OGRFeature feat0_overwrite(layer->GetLayerDefn());
    feat0_overwrite.SetFID(0);
    feat0_overwrite.SetField("val", 999);
    EXPECT_EQ(OGRERR_NONE, layer->ISetFeature(&feat0_overwrite));
    std::unique_ptr<OGRFeature> read_feat(layer->GetFeature(0));
    ASSERT_THAT(read_feat, NotNull());
    EXPECT_EQ(999, read_feat->GetFieldAsInteger("val"));
  }

  // 4. Geometry without SRS gets layer's SRS assigned.
  {
    OGRFeature feat_geom(layer->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    EXPECT_THAT(pt->getSpatialReference(), IsNull());
    feat_geom.SetGeometry(pt.get());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat_geom));
    std::unique_ptr<OGRFeature> read_feat(layer->GetFeature(feat_geom.GetFID()));
    ASSERT_THAT(read_feat, NotNull());
    const OGRGeometry* read_geom = read_feat->GetGeometryRef();
    ASSERT_THAT(read_geom, NotNull());
    ASSERT_THAT(read_geom->getSpatialReference(), NotNull());
    EXPECT_THAT(*read_geom->getSpatialReference(), IsSameAs(wgs84));
  }

  // 5. ICreateFeature with a pre-set FID that already exists in m_papoFeatures.
  {
    OGRFeature dup_fid_feat(layer->GetLayerDefn());
    dup_fid_feat.SetFID(0);  // Already exists!
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&dup_fid_feat));
    // FID should have been re-assigned to a new valid FID.
    EXPECT_NE(0, dup_fid_feat.GetFID());
  }

  // 6. ICreateFeature with a non-contiguous FID to introduce holes (m_bHasHoles = true).
  {
    OGRFeature hole_feat(layer->GetLayerDefn());
    hole_feat.SetFID(20);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&hole_feat));

    // Iteration over layer with holes exercises `if (!poFeature) continue;` in GetNextFeature.
    layer->ResetReading();
    int hole_iter_count = 0;
    while (std::unique_ptr<OGRFeature> f =
               std::unique_ptr<OGRFeature>(layer->GetNextFeature())) {
      ++hole_iter_count;
    }
    EXPECT_GT(hole_iter_count, 0);
  }

  // 7. GetFeature checks.
  EXPECT_THAT(layer->GetFeature(-1), IsNull());
  EXPECT_THAT(layer->GetFeature(1000), IsNull());
  EXPECT_THAT(layer->GetFeature(15), IsNull());  // Hole index
  std::unique_ptr<OGRFeature> valid_f(layer->GetFeature(20));
  ASSERT_THAT(valid_f, NotNull());
  EXPECT_EQ(20, valid_f->GetFID());

  // 8. DeleteFeature checks.
  EXPECT_EQ(OGRERR_FAILURE, layer->DeleteFeature(-1));
  EXPECT_EQ(OGRERR_FAILURE, layer->DeleteFeature(1000));
  EXPECT_EQ(OGRERR_FAILURE, layer->DeleteFeature(15));  // Already empty
  EXPECT_EQ(OGRERR_NONE, layer->DeleteFeature(20));
  EXPECT_THAT(layer->GetFeature(20), IsNull());

  // 9. When not updatable, mutation operations fail.
  layer->SetUpdatable(false);
  OGRFeature mut_f(layer->GetLayerDefn());
  EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&mut_f));
  EXPECT_EQ(OGRERR_FAILURE, layer->ISetFeature(&mut_f));
  EXPECT_EQ(OGRERR_FAILURE, layer->DeleteFeature(0));
  layer->SetUpdatable(true);

  // 10. SetNextByIndex and GetNextFeature on a contiguous layer.
  {
    OGRMemLayer contig_layer("contig", nullptr, wkbNone);
    for (int i = 0; i < 5; ++i) {
      OGRFeature f(contig_layer.GetLayerDefn());
      EXPECT_EQ(OGRERR_NONE, contig_layer.CreateFeature(&f));
    }
    EXPECT_EQ(5, contig_layer.GetFeatureCount(FALSE));
    EXPECT_EQ(TRUE, contig_layer.TestCapability(OLCFastSetNextByIndex));

    // Valid index
    EXPECT_EQ(OGRERR_NONE, contig_layer.SetNextByIndex(2));
    std::unique_ptr<OGRFeature> f(contig_layer.GetNextFeature());
    ASSERT_THAT(f, NotNull());
    EXPECT_EQ(2, f->GetFID());

    // Out of range index
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, contig_layer.SetNextByIndex(10));
    EXPECT_THAT(contig_layer.GetNextFeature(), IsNull());

    // Negative index
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, contig_layer.SetNextByIndex(-1));
    EXPECT_THAT(contig_layer.GetNextFeature(), IsNull());

    // Reset and iterate all
    contig_layer.ResetReading();
    int count = 0;
    while (std::unique_ptr<OGRFeature> feat =
               std::unique_ptr<OGRFeature>(contig_layer.GetNextFeature())) {
      ++count;
    }
    EXPECT_EQ(5, count);
  }

  // 11. SetNextByIndex delegates when holes or filters exist.
  {
    EXPECT_EQ(OGRERR_NONE, layer->SetNextByIndex(0));
  }

  // 12. Spatial and attribute filters with GetFeatureCount and GetNextFeature.
  {
    OGRMemLayer filter_layer("filt", nullptr, wkbPoint);
    OGRFieldDefn fld_num("num", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, filter_layer.CreateField(&fld_num));

    for (int i = 0; i < 5; ++i) {
      OGRFeature f(filter_layer.GetLayerDefn());
      f.SetField("num", i);
      OGRPoint pt(static_cast<double>(i), static_cast<double>(i));
      f.SetGeometry(&pt);
      EXPECT_EQ(OGRERR_NONE, filter_layer.CreateFeature(&f));
    }

    EXPECT_EQ(5, filter_layer.GetFeatureCount(FALSE));

    filter_layer.SetSpatialFilterRect(0.5, 0.5, 2.5, 2.5);
    EXPECT_EQ(2, filter_layer.GetFeatureCount(TRUE));

    filter_layer.SetAttributeFilter("num = 1");
    EXPECT_EQ(1, filter_layer.GetFeatureCount(TRUE));

    filter_layer.ResetReading();
    std::unique_ptr<OGRFeature> matched(filter_layer.GetNextFeature());
    ASSERT_THAT(matched, NotNull());
    EXPECT_EQ(1, matched->GetFieldAsInteger("num"));
    EXPECT_THAT(filter_layer.GetNextFeature(), IsNull());

    filter_layer.SetSpatialFilter(nullptr);
    filter_layer.SetAttributeFilter(nullptr);
    EXPECT_EQ(5, filter_layer.GetFeatureCount(FALSE));
  }

  // 13. Layer destructor reads features to trigger m_nFeaturesRead > 0 debug branch.
  layer->ResetReading();
  std::unique_ptr<OGRFeature> read1(layer->GetNextFeature());
  ASSERT_THAT(read1, NotNull());
  layer.reset();  // Destructor runs with m_nFeaturesRead > 0 and frees m_papoFeatures.
}

// -----------------------------------------------------------------------------
// 4. Map Mode Conversion and Operations
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, MapModeConversionAndOperations) {
  // 1. Array mode to Map mode conversion via large FID gap.
  {
    OGRMemLayer layer("convert_layer", nullptr, wkbNone);
    OGRFieldDefn fld("txt", OFTString);
    ASSERT_EQ(OGRERR_NONE, layer.CreateField(&fld));

    // Populate array with FIDs 0 and 2 (hole at 1).
    OGRFeature f0(layer.GetLayerDefn());
    f0.SetFID(0);
    f0.SetField("txt", "zero");
    EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f0));

    OGRFeature f2(layer.GetLayerDefn());
    f2.SetFID(2);
    f2.SetField("txt", "two");
    EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f2));

    // Trigger conversion: nFID > 100000 && nFID > m_nMaxFeatureCount + 1000.
    constexpr GIntBig kLargeFid = 105000;
    OGRFeature f_large(layer.GetLayerDefn());
    f_large.SetFID(kLargeFid);
    f_large.SetField("txt", "large");
    EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f_large));

    EXPECT_EQ(3, layer.GetFeatureCount(FALSE));
    EXPECT_EQ(FALSE, layer.TestCapability(OLCFastSetNextByIndex));
    std::unique_ptr<OGRFeature> rf0(layer.GetFeature(0));
    ASSERT_THAT(rf0, NotNull());
    EXPECT_STREQ("zero", rf0->GetFieldAsString("txt"));
    std::unique_ptr<OGRFeature> rf2(layer.GetFeature(2));
    ASSERT_THAT(rf2, NotNull());
    EXPECT_STREQ("two", rf2->GetFieldAsString("txt"));
    std::unique_ptr<OGRFeature> rfl(layer.GetFeature(kLargeFid));
    ASSERT_THAT(rfl, NotNull());
    EXPECT_STREQ("large", rfl->GetFieldAsString("txt"));
  }

  // 2. Start layer directly in map mode with first feature FID > 100000.
  {
    OGRMemLayer layer("direct_map", nullptr, wkbNone);
    {
      OGRFeature f_init(layer.GetLayerDefn());
      f_init.SetFID(200000);
      EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f_init));

      // Now insert FID 0 into map.
      OGRFeature f0(layer.GetLayerDefn());
      f0.SetFID(0);
      EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f0));

      // Insert with OGRNullFID while in map mode (exercises m_oMapFeatures loop).
      OGRFeature f_null(layer.GetLayerDefn());
      f_null.SetFID(OGRNullFID);
      EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f_null));
      EXPECT_EQ(1, f_null.GetFID());

      // Overwrite existing FID in map mode.
      OGRFeature f_over(layer.GetLayerDefn());
      f_over.SetFID(200000);
      EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(&f_over));

      // ICreateFeature with existing FID in map mode (resets to OGRNullFID and creates).
      OGRFeature f_dup(layer.GetLayerDefn());
      f_dup.SetFID(200000);
      EXPECT_EQ(OGRERR_NONE, layer.CreateFeature(&f_dup));
      EXPECT_NE(200000, f_dup.GetFID());

      // Non-existing FID checks in map mode.
      EXPECT_THAT(layer.GetFeature(-5), IsNull());
      EXPECT_THAT(layer.GetFeature(999999), IsNull());
      EXPECT_EQ(OGRERR_FAILURE, layer.DeleteFeature(-5));
      EXPECT_EQ(OGRERR_FAILURE, layer.DeleteFeature(999999));

      // Delete existing feature in map mode.
      EXPECT_EQ(OGRERR_NONE, layer.DeleteFeature(200000));
      EXPECT_THAT(layer.GetFeature(200000), IsNull());

      // Iteration via OGRMemLayerIteratorMap in GetNextFeature.
      layer.ResetReading();
      int count = 0;
      while (std::unique_ptr<OGRFeature> f =
                 std::unique_ptr<OGRFeature>(layer.GetNextFeature())) {
        ++count;
      }
      EXPECT_EQ(3, count);
    }

    // CreateField while in map mode exercises OGRMemLayerIteratorMap in CreateField.
    OGRFieldDefn new_fld("extra", OFTReal);
    EXPECT_EQ(OGRERR_NONE, layer.CreateField(&new_fld));
  }
}

// -----------------------------------------------------------------------------
// 5. Upsert and UpdateFeature
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, UpsertAndUpdateFeature) {
  OGRMemLayer layer("upsert_update", nullptr, wkbPoint);
  OGRFieldDefn fld1("fld1", OFTInteger);
  OGRFieldDefn fld2("fld2", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&fld1));
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&fld2));

  // 1. UpsertFeature: insert new feature when FID does not exist.
  OGRFeature f1(layer.GetLayerDefn());
  f1.SetFID(10);
  f1.SetField("fld1", 100);
  f1.SetField("fld2", "initial");
  EXPECT_EQ(OGRERR_NONE, layer.IUpsertFeature(&f1));
  EXPECT_EQ(1, layer.GetFeatureCount(FALSE));

  // 2. UpsertFeature: update feature when FID exists.
  f1.SetField("fld1", 200);
  EXPECT_EQ(OGRERR_NONE, layer.IUpsertFeature(&f1));
  std::unique_ptr<OGRFeature> read1(layer.GetFeature(10));
  ASSERT_THAT(read1, NotNull());
  EXPECT_EQ(200, read1->GetFieldAsInteger("fld1"));

  // 3. UpsertFeature fails when layer is not updatable.
  layer.SetUpdatable(false);
  EXPECT_EQ(OGRERR_FAILURE, layer.IUpsertFeature(&f1));
  layer.SetUpdatable(true);

  // 4. UpdateFeature on missing FID returns OGRERR_NON_EXISTING_FEATURE.
  OGRFeature missing_f(layer.GetLayerDefn());
  missing_f.SetFID(999);
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE,
            layer.IUpdateFeature(&missing_f, 0, nullptr, 0, nullptr, false));

  // 5. UpdateFeature on existing FID: update attribute, geometry, and style string.
  OGRFeature update_f(layer.GetLayerDefn());
  update_f.SetFID(10);
  update_f.SetField("fld2", "updated_str");
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  update_f.SetGeometryDirectly(pt.release());
  update_f.SetStyleString("PEN(c:#FF0000)");

  constexpr int kUpdatedFieldIdx[] = {1};
  constexpr int kUpdatedGeomIdx[] = {0};
  EXPECT_EQ(OGRERR_NONE,
            layer.IUpdateFeature(&update_f, 1, kUpdatedFieldIdx, 1,
                                 kUpdatedGeomIdx, /*bUpdateStyleString=*/true));

  std::unique_ptr<OGRFeature> read_updated(layer.GetFeature(10));
  ASSERT_THAT(read_updated, NotNull());
  EXPECT_EQ(200, read_updated->GetFieldAsInteger("fld1"));  // Unchanged
  EXPECT_STREQ("updated_str", read_updated->GetFieldAsString("fld2"));
  EXPECT_STREQ("PEN(c:#FF0000)", read_updated->GetStyleString());
  ASSERT_THAT(read_updated->GetGeometryRef(), NotNull());
  EXPECT_DOUBLE_EQ(1.0, read_updated->GetGeometryRef()->toPoint()->getX());

  // 6. UpdateFeature fails when layer is not updatable.
  layer.SetUpdatable(false);
  EXPECT_EQ(OGRERR_FAILURE,
            layer.IUpdateFeature(&update_f, 1, kUpdatedFieldIdx, 1,
                                 kUpdatedGeomIdx, false));
}

// -----------------------------------------------------------------------------
// 6. Create, Delete, Reorder, and Alter Fields
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, CreateField) {
  OGRMemLayer layer("create_fld_layer", nullptr, wkbNone);

  // 1. CreateField on empty layer.
  OGRFieldDefn f_int("int_fld", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, layer.CreateField(&f_int));

  // Populate two features: one with values set, one with unset/null.
  {
    OGRFeature f0(layer.GetLayerDefn());
    f0.SetField("int_fld", 42);
    EXPECT_EQ(OGRERR_NONE, layer.CreateFeature(&f0));

    OGRFeature f1(layer.GetLayerDefn());
    EXPECT_EQ(OGRERR_NONE, layer.CreateFeature(&f1));
  }

  // 2. CreateField on populated layer.
  OGRFieldDefn f_str("str_fld", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer.CreateField(&f_str));
  OGRFieldDefn f_real("real_fld", OFTReal);
  EXPECT_EQ(OGRERR_NONE, layer.CreateField(&f_real));

  // Set fields on feature 0.
  std::unique_ptr<OGRFeature> rf0(layer.GetFeature(0));
  ASSERT_THAT(rf0, NotNull());
  rf0->SetField("str_fld", "hello");
  rf0->SetField("real_fld", 3.14);
  EXPECT_EQ(OGRERR_NONE, layer.ISetFeature(rf0.get()));

  // CreateField fails when layer is not updatable.
  layer.SetUpdatable(false);
  OGRFieldDefn f_blocked("blocked", OFTInteger);
  EXPECT_EQ(OGRERR_FAILURE, layer.CreateField(&f_blocked));
}

TEST(OgrMemLayerTest, DeleteField) {
  OGRMemLayer layer("del_layer", nullptr, wkbNone);
  OGRFieldDefn f_int("int_fld", OFTInteger);
  OGRFieldDefn f_str("str_fld", OFTString);
  OGRFieldDefn f_real("real_fld", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_int));
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_str));
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_real));

  {
    OGRFeature f0(layer.GetLayerDefn());
    f0.SetField("int_fld", 42);
    f0.SetField("str_fld", "hello");
    f0.SetField("real_fld", 3.14);
    ASSERT_EQ(OGRERR_NONE, layer.CreateFeature(&f0));

    OGRFeature f1(layer.GetLayerDefn());
    ASSERT_EQ(OGRERR_NONE, layer.CreateFeature(&f1));
  }

  // Invalid index and not updatable.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE, layer.DeleteField(-1));
    EXPECT_EQ(OGRERR_FAILURE, layer.DeleteField(10));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  layer.SetUpdatable(false);
  EXPECT_EQ(OGRERR_FAILURE, layer.DeleteField(1));
  layer.SetUpdatable(true);

  // Delete middle field (str_fld at index 1).
  EXPECT_EQ(OGRERR_NONE, layer.DeleteField(1));
  EXPECT_EQ(2, layer.GetLayerDefn()->GetFieldCount());
  EXPECT_STREQ("int_fld", layer.GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("real_fld", layer.GetLayerDefn()->GetFieldDefn(1)->GetNameRef());

  // Delete last field (real_fld at index 1).
  EXPECT_EQ(OGRERR_NONE, layer.DeleteField(1));
  EXPECT_EQ(1, layer.GetLayerDefn()->GetFieldCount());
  EXPECT_STREQ("int_fld", layer.GetLayerDefn()->GetFieldDefn(0)->GetNameRef());

  std::unique_ptr<OGRFeature> rf0(layer.GetFeature(0));
  ASSERT_THAT(rf0, NotNull());
  EXPECT_EQ(42, rf0->GetFieldAsInteger("int_fld"));
}

TEST(OgrMemLayerTest, ReorderFields) {
  // Empty layer returns NONE.
  {
    OGRMemLayer empty_fld_layer("empty_flds", nullptr, wkbNone);
    int dummy_map[1] = {0};
    EXPECT_EQ(OGRERR_NONE, empty_fld_layer.ReorderFields(dummy_map));
  }

  OGRMemLayer layer("reorder_layer", nullptr, wkbNone);
  OGRFieldDefn f_int("int_fld", OFTInteger);
  OGRFieldDefn f_str("str_fld", OFTString);
  OGRFieldDefn f_real("real_fld", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_int));
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_str));
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_real));

  {
    OGRFeature f0(layer.GetLayerDefn());
    f0.SetField("int_fld", 42);
    f0.SetField("str_fld", "hello");
    f0.SetField("real_fld", 3.14);
    ASSERT_EQ(OGRERR_NONE, layer.CreateFeature(&f0));
  }

  // Invalid permutation fails.
  {
    int bad_map[3] = {0, 0, 1};
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(OGRERR_NONE, layer.ReorderFields(bad_map));
  }

  // Not updatable fails.
  layer.SetUpdatable(false);
  int good_map[3] = {2, 0, 1};
  EXPECT_EQ(OGRERR_FAILURE, layer.ReorderFields(good_map));
  layer.SetUpdatable(true);

  // Valid permutation: {2, 0, 1}
  // new 0 -> old 2 (real_fld)
  // new 1 -> old 0 (int_fld)
  // new 2 -> old 1 (str_fld)
  EXPECT_EQ(OGRERR_NONE, layer.ReorderFields(good_map));
  EXPECT_STREQ("real_fld", layer.GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("int_fld", layer.GetLayerDefn()->GetFieldDefn(1)->GetNameRef());
  EXPECT_STREQ("str_fld", layer.GetLayerDefn()->GetFieldDefn(2)->GetNameRef());

  std::unique_ptr<OGRFeature> rf0(layer.GetFeature(0));
  ASSERT_THAT(rf0, NotNull());
  EXPECT_DOUBLE_EQ(3.14, rf0->GetFieldAsDouble("real_fld"));
  EXPECT_EQ(42, rf0->GetFieldAsInteger("int_fld"));
  EXPECT_STREQ("hello", rf0->GetFieldAsString("str_fld"));
}

TEST(OgrMemLayerTest, AlterFieldDefn) {
  OGRMemLayer layer("alter_layer", nullptr, wkbNone);
  OGRFieldDefn f_real("real_fld", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer.CreateField(&f_real));

  // Invalid index and not updatable.
  OGRFieldDefn dummy_alt("alt", OFTInteger);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE, layer.AlterFieldDefn(-1, &dummy_alt, 0));
    EXPECT_EQ(OGRERR_FAILURE, layer.AlterFieldDefn(10, &dummy_alt, 0));
  }
  layer.SetUpdatable(false);
  EXPECT_EQ(OGRERR_FAILURE, layer.AlterFieldDefn(0, &dummy_alt, 0));
  layer.SetUpdatable(true);

  // Alter name, width, and precision.
  OGRFieldDefn new_name_defn("renamed_real", OFTReal);
  new_name_defn.SetWidth(12);
  new_name_defn.SetPrecision(4);
  EXPECT_EQ(OGRERR_NONE,
            layer.AlterFieldDefn(0, &new_name_defn,
                                 ALTER_NAME_FLAG | ALTER_WIDTH_PRECISION_FLAG));
  EXPECT_STREQ("renamed_real",
               layer.GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(12, layer.GetLayerDefn()->GetFieldDefn(0)->GetWidth());
  EXPECT_EQ(4, layer.GetLayerDefn()->GetFieldDefn(0)->GetPrecision());

  // Alter field types on populated layers.
  // Date -> DateTime.
  {
    OGRMemLayer date_layer("date_l", nullptr, wkbNone);
    OGRFieldDefn dt_fld("dt", OFTDate);
    ASSERT_EQ(OGRERR_NONE, date_layer.CreateField(&dt_fld));
    OGRFieldDefn dttm_fld("dt", OFTDateTime);
    EXPECT_EQ(OGRERR_NONE,
              date_layer.AlterFieldDefn(0, &dttm_fld, ALTER_TYPE_FLAG));
    EXPECT_EQ(OFTDateTime,
              date_layer.GetLayerDefn()->GetFieldDefn(0)->GetType());
  }

  // Integer -> Integer64.
  {
    OGRMemLayer int_layer("int_l", nullptr, wkbNone);
    OGRFieldDefn ifld("val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, int_layer.CreateField(&ifld));
    OGRFeature f(int_layer.GetLayerDefn());
    f.SetField("val", 123);
    EXPECT_EQ(OGRERR_NONE, int_layer.CreateFeature(&f));
    OGRFeature f_null(int_layer.GetLayerDefn());
    EXPECT_EQ(OGRERR_NONE, int_layer.CreateFeature(&f_null));

    OGRFieldDefn i64fld("val", OFTInteger64);
    EXPECT_EQ(OGRERR_NONE,
              int_layer.AlterFieldDefn(0, &i64fld, ALTER_TYPE_FLAG));
    std::unique_ptr<OGRFeature> r(int_layer.GetFeature(0));
    ASSERT_THAT(r, NotNull());
    EXPECT_EQ(123, r->GetFieldAsInteger64("val"));
  }

  // Integer -> Real.
  {
    OGRMemLayer int_layer("int_to_real", nullptr, wkbNone);
    OGRFieldDefn ifld("val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, int_layer.CreateField(&ifld));
    OGRFeature f(int_layer.GetLayerDefn());
    f.SetField("val", 456);
    EXPECT_EQ(OGRERR_NONE, int_layer.CreateFeature(&f));

    OGRFieldDefn rfld("val", OFTReal);
    EXPECT_EQ(OGRERR_NONE,
              int_layer.AlterFieldDefn(0, &rfld, ALTER_TYPE_FLAG));
    std::unique_ptr<OGRFeature> r(int_layer.GetFeature(0));
    ASSERT_THAT(r, NotNull());
    EXPECT_DOUBLE_EQ(456.0, r->GetFieldAsDouble("val"));
  }

  // Integer64 -> Real.
  {
    OGRMemLayer i64_layer("i64_to_real", nullptr, wkbNone);
    OGRFieldDefn ifld("val", OFTInteger64);
    ASSERT_EQ(OGRERR_NONE, i64_layer.CreateField(&ifld));
    OGRFeature f(i64_layer.GetLayerDefn());
    f.SetField("val", static_cast<GIntBig>(789));
    EXPECT_EQ(OGRERR_NONE, i64_layer.CreateFeature(&f));

    OGRFieldDefn rfld("val", OFTReal);
    EXPECT_EQ(OGRERR_NONE,
              i64_layer.AlterFieldDefn(0, &rfld, ALTER_TYPE_FLAG));
    std::unique_ptr<OGRFeature> r(i64_layer.GetFeature(0));
    ASSERT_THAT(r, NotNull());
    EXPECT_DOUBLE_EQ(789.0, r->GetFieldAsDouble("val"));
  }

  // Real -> Integer (unsupported, returns failure).
  {
    OGRMemLayer real_layer("real_to_int", nullptr, wkbNone);
    OGRFieldDefn rfld("val", OFTReal);
    ASSERT_EQ(OGRERR_NONE, real_layer.CreateField(&rfld));

    OGRFieldDefn ifld("val", OFTInteger);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE,
              real_layer.AlterFieldDefn(0, &ifld, ALTER_TYPE_FLAG));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Real -> String (supported conversion).
  {
    OGRMemLayer real_layer("real_to_str", nullptr, wkbNone);
    OGRFieldDefn rfld("val", OFTReal);
    ASSERT_EQ(OGRERR_NONE, real_layer.CreateField(&rfld));
    {
      OGRFeature f(real_layer.GetLayerDefn());
      f.SetField("val", 3.5);
      EXPECT_EQ(OGRERR_NONE, real_layer.CreateFeature(&f));
    }

    OGRFieldDefn sfld("val", OFTString);
    EXPECT_EQ(OGRERR_NONE,
              real_layer.AlterFieldDefn(0, &sfld, ALTER_TYPE_FLAG));
    std::unique_ptr<OGRFeature> r(real_layer.GetFeature(0));
    ASSERT_THAT(r, NotNull());
    EXPECT_STREQ("3.5", r->GetFieldAsString("val"));
  }
}

// -----------------------------------------------------------------------------
// 7. Create and Alter Geom Fields
// -----------------------------------------------------------------------------

TEST(OgrMemLayerTest, CreateAndAlterGeomFields) {
  OGRMemLayer layer("geom_layer", nullptr, wkbNone);

  // 1. CreateGeomField on empty layer.
  OGRGeomFieldDefn g1("pt_geom", wkbPoint);
  EXPECT_EQ(OGRERR_NONE, layer.CreateGeomField(&g1));

  // Populate a feature with geom.
  {
    OGRFeature f0(layer.GetLayerDefn());
    OGRPoint pt0(1.0, 2.0);
    f0.SetGeomFieldDirectly(0, pt0.clone());
    EXPECT_EQ(OGRERR_NONE, layer.CreateFeature(&f0));
  }

  // 2. CreateGeomField on populated layer (triggers RemapGeomFields).
  OGRGeomFieldDefn g2("poly_geom", wkbPolygon);
  EXPECT_EQ(OGRERR_NONE, layer.CreateGeomField(&g2));
  EXPECT_EQ(2, layer.GetLayerDefn()->GetGeomFieldCount());

  // Check existing feature retained geometry.
  std::unique_ptr<OGRFeature> rf0(layer.GetFeature(0));
  ASSERT_THAT(rf0, NotNull());
  ASSERT_THAT(rf0->GetGeomFieldRef(0), NotNull());
  EXPECT_DOUBLE_EQ(1.0, rf0->GetGeomFieldRef(0)->toPoint()->getX());

  // Fails when not updatable.
  layer.SetUpdatable(false);
  OGRGeomFieldDefn g3("line_geom", wkbLineString);
  EXPECT_EQ(OGRERR_FAILURE, layer.CreateGeomField(&g3));
  layer.SetUpdatable(true);

  // 3. AlterGeomFieldDefn: invalid index and not updatable.
  OGRGeomFieldDefn dummy_geom("dummy", wkbPoint);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE, layer.AlterGeomFieldDefn(-1, &dummy_geom, 0));
    EXPECT_EQ(OGRERR_FAILURE, layer.AlterGeomFieldDefn(5, &dummy_geom, 0));
  }
  layer.SetUpdatable(false);
  EXPECT_EQ(OGRERR_FAILURE, layer.AlterGeomFieldDefn(0, &dummy_geom, 0));
  layer.SetUpdatable(true);

  // Alter Name and Nullable.
  OGRGeomFieldDefn renamed_geom("new_pt_geom", wkbPoint);
  renamed_geom.SetNullable(false);
  EXPECT_EQ(
      OGRERR_NONE,
      layer.AlterGeomFieldDefn(0, &renamed_geom,
                               ALTER_GEOM_FIELD_DEFN_NAME_FLAG |
                                   ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG));
  EXPECT_STREQ("new_pt_geom",
               layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef());
  EXPECT_FALSE(layer.GetLayerDefn()->GetGeomFieldDefn(0)->IsNullable());

  // Alter Type: wkbNone fails; valid type succeeds.
  OGRGeomFieldDefn none_type_geom("none_geom", wkbNone);
  EXPECT_EQ(
      OGRERR_FAILURE,
      layer.AlterGeomFieldDefn(0, &none_type_geom,
                               ALTER_GEOM_FIELD_DEFN_TYPE_FLAG));

  OGRGeomFieldDefn poly_type_geom("poly", wkbPolygon);
  EXPECT_EQ(
      OGRERR_NONE,
      layer.AlterGeomFieldDefn(0, &poly_type_geom,
                               ALTER_GEOM_FIELD_DEFN_TYPE_FLAG));
  EXPECT_EQ(wkbPolygon,
            layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetType());

  // 4. Alter SRS and CoordinateEpoch branches.
  const OGRSpatialReference wgs84 = GetWgs84Srs();

  // 4a. ALTER_GEOM_FIELD_DEFN_SRS_FLAG when old SRS is null.
  {
    OGRGeomFieldDefn srs_geom("srs_g", wkbPoint);
    srs_geom.SetSpatialRef(&wgs84);
    EXPECT_EQ(OGRERR_NONE,
              layer.AlterGeomFieldDefn(0, &srs_geom,
                                       ALTER_GEOM_FIELD_DEFN_SRS_FLAG));
    const auto* cur_srs =
        layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef();
    ASSERT_THAT(cur_srs, NotNull());
    EXPECT_DOUBLE_EQ(0.0, cur_srs->GetCoordinateEpoch());
  }

  // 4b. ALTER_GEOM_FIELD_DEFN_SRS_FLAG when old SRS has epoch 2021.5.
  {
    OGRSpatialReference srs_epoch = wgs84;
    srs_epoch.SetCoordinateEpoch(2021.5);
    OGRGeomFieldDefn srs_old_epoch("srs_old", wkbPoint);
    srs_old_epoch.SetSpatialRef(&srs_epoch);
    EXPECT_EQ(OGRERR_NONE,
              layer.AlterGeomFieldDefn(0, &srs_old_epoch,
                                       ALTER_GEOM_FIELD_DEFN_SRS_FLAG |
                                           ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG));
  }

  // Change SRS with ALTER_GEOM_FIELD_DEFN_SRS_FLAG alone: preserves 2021.5 epoch.
  {
    OGRGeomFieldDefn srs_new("srs_new", wkbPoint);
    srs_new.SetSpatialRef(&wgs84);
    EXPECT_EQ(OGRERR_NONE,
              layer.AlterGeomFieldDefn(0, &srs_new,
                                       ALTER_GEOM_FIELD_DEFN_SRS_FLAG));
    const auto* cur_srs =
        layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef();
    ASSERT_THAT(cur_srs, NotNull());
    EXPECT_DOUBLE_EQ(2021.5, cur_srs->GetCoordinateEpoch());
  }

  // Clear SRS by setting new field defn with nullptr SRS.
  {
    OGRGeomFieldDefn srs_null("srs_null", wkbPoint);
    EXPECT_EQ(OGRERR_NONE,
              layer.AlterGeomFieldDefn(0, &srs_null,
                                       ALTER_GEOM_FIELD_DEFN_SRS_FLAG));
    EXPECT_THAT(layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef(),
                IsNull());
  }

  // 4c. ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG alone when old and new SRS are non-null.
  {
    OGRGeomFieldDefn srs_geom("srs_g", wkbPoint);
    srs_geom.SetSpatialRef(&wgs84);
    EXPECT_EQ(OGRERR_NONE,
              layer.AlterGeomFieldDefn(0, &srs_geom,
                                       ALTER_GEOM_FIELD_DEFN_SRS_FLAG));

    OGRSpatialReference srs_target = wgs84;
    srs_target.SetCoordinateEpoch(2024.0);
    OGRGeomFieldDefn srs_epoch_update("srs_up", wkbPoint);
    srs_epoch_update.SetSpatialRef(&srs_target);
    EXPECT_EQ(
        OGRERR_NONE,
        layer.AlterGeomFieldDefn(0, &srs_epoch_update,
                                 ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG));
    const auto* cur_srs =
        layer.GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef();
    ASSERT_THAT(cur_srs, NotNull());
    EXPECT_DOUBLE_EQ(2024.0, cur_srs->GetCoordinateEpoch());
  }
}

}  // namespace
}  // namespace autotest2
