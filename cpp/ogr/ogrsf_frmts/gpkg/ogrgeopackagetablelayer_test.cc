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

// Unit tests for OGRGeoPackageTableLayer.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/ogrgeopackagetablelayer.cpp
//   https://gdal.org/drivers/vector/gpkg.html

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogr_geopackage.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogrgeopackageutility.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_md5.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

constexpr char kTestGpkg[] = "/vsimem/ogrgeopackagetablelayer_test.gpkg";

class OGRGeoPackageTableLayerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterOGRGeoPackage(); }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void SetUp() override {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, driver);
    dataset_.reset(driver->Create(kTestGpkg, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, dataset_);
  }

  void TearDown() override {
    dataset_.reset();
    VSIUnlink(kTestGpkg);
  }

  std::unique_ptr<GDALDataset> dataset_;
};

// Tests table creation options, capabilities, and metadata items.
TEST_F(OGRGeoPackageTableLayerTest, CreationOptionsAndCapabilities) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const options[] = {
      "FID=my_fid",
      "GEOMETRY_NAME=the_geom",
      "SPATIAL_INDEX=YES",
      "GEOMETRY_NULLABLE=NO",
      "DESCRIPTION=A test layer description",
      "IDENTIFIER=TestLayerIdentifier",
      "PRECISION=NO",
      "DISCARD_COORD_LSB=YES",
      "UNDO_DISCARD_COORD_LSB_ON_READING=YES",
      nullptr,
  };

  OGRLayer* layer = dataset_->CreateLayer("t_options", &srs, wkbPoint,
                                          const_cast<char**>(options));
  ASSERT_NE(nullptr, layer);

  EXPECT_STREQ("my_fid", layer->GetFIDColumn());
  EXPECT_STREQ("the_geom", layer->GetGeometryColumn());

  EXPECT_TRUE(layer->TestCapability(OLCRandomRead));
  EXPECT_TRUE(layer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(layer->TestCapability(OLCRandomWrite));
  EXPECT_TRUE(layer->TestCapability(OLCFastSpatialFilter));
  EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(layer->TestCapability(OLCFastGetExtent));
  EXPECT_TRUE(layer->TestCapability(OLCFastGetExtent3D));
  EXPECT_TRUE(layer->TestCapability(OLCCreateField));
  EXPECT_FALSE(layer->TestCapability(OLCCreateGeomField));
  EXPECT_TRUE(layer->TestCapability(OLCDeleteField));
  EXPECT_TRUE(layer->TestCapability(OLCReorderFields));
  EXPECT_TRUE(layer->TestCapability(OLCAlterFieldDefn));
  EXPECT_TRUE(layer->TestCapability(OLCAlterGeomFieldDefn));
  EXPECT_TRUE(layer->TestCapability(OLCTransactions));
  EXPECT_TRUE(layer->TestCapability(OLCUpsertFeature));
  EXPECT_TRUE(layer->TestCapability(OLCUpdateFeature));
  EXPECT_TRUE(layer->TestCapability(OLCFastSetNextByIndex));
  EXPECT_TRUE(layer->TestCapability(OLCRename));
  EXPECT_TRUE(layer->TestCapability(OLCIgnoreFields));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCFastGetArrowStream));
  EXPECT_TRUE(layer->TestCapability(OLCZGeometries));
  EXPECT_TRUE(layer->TestCapability(OLCMeasuredGeometries));
  EXPECT_TRUE(layer->TestCapability(OLCCurveGeometries));
  EXPECT_FALSE(layer->TestCapability("NON_EXISTENT_CAPABILITY"));

  // Also test aspatial layer with ASPATIAL_VARIANT=NOT_REGISTERED
  const char* const aspatial_options[] = {
      "ASPATIAL_VARIANT=NOT_REGISTERED",
      nullptr,
  };
  OGRLayer* aspatial_layer = dataset_->CreateLayer(
      "t_aspatial", nullptr, wkbNone, const_cast<char**>(aspatial_options));
  ASSERT_NE(nullptr, aspatial_layer);
  EXPECT_STREQ("", aspatial_layer->GetGeometryColumn());
}

// Tests creating fields of all data types, subtypes, domains, defaults, and
// constraints.
TEST_F(OGRGeoPackageTableLayerTest,
       ReadTableDefinitionAllColumnTypesAndReopen) {
  OGRLayer* layer =
      dataset_->CreateLayer("all_types", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  // Field domains
  std::vector<OGRCodedValue> coded_values = {
      OGRCodedValue{CPLStrdup("1"), CPLStrdup("One")},
      OGRCodedValue{CPLStrdup("2"), CPLStrdup("Two")},
  };
  auto oDomain = std::make_unique<OGRCodedFieldDomain>(
      "my_coded_domain", "desc", OFTInteger, OFSTNone, std::move(coded_values));
  std::string failure_reason;
  EXPECT_TRUE(dataset_->AddFieldDomain(std::move(oDomain), failure_reason));

  OGRField sMin;
  sMin.Real = 0.0;
  OGRField sMax;
  sMax.Real = 100.0;
  auto oRangeDomain = std::make_unique<OGRRangeFieldDomain>(
      "my_range_domain", "desc", OFTReal, OFSTNone, sMin, true, sMax, true);
  EXPECT_TRUE(
      dataset_->AddFieldDomain(std::move(oRangeDomain), failure_reason));

  auto oGlobDomain = std::make_unique<OGRGlobFieldDomain>(
      "my_glob_domain", "desc", OFTString, OFSTNone, "*@google.com");
  EXPECT_TRUE(dataset_->AddFieldDomain(std::move(oGlobDomain), failure_reason));

  // OFTInteger with variants
  {
    OGRFieldDefn f("f_int", OFTInteger);
    f.SetDefault("42");
    f.SetNullable(false);
    f.SetUnique(true);
    f.SetDomainName("my_coded_domain");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_bool", OFTInteger);
    f.SetSubType(OFSTBoolean);
    f.SetDefault("1");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_int16", OFTInteger);
    f.SetSubType(OFSTInt16);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }

  // OFTInteger64
  {
    OGRFieldDefn f("f_int64", OFTInteger64);
    f.SetComment("64-bit int field");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }

  // OFTReal with variants
  {
    OGRFieldDefn f("f_real", OFTReal);
    f.SetDomainName("my_range_domain");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_float32", OFTReal);
    f.SetSubType(OFSTFloat32);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }

  // OFTString with variants
  {
    OGRFieldDefn f("f_str", OFTString);
    f.SetWidth(25);
    f.SetDomainName("my_glob_domain");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_json", OFTString);
    f.SetSubType(OFSTJSON);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_uuid", OFTString);
    f.SetSubType(OFSTUUID);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }

  // OFTBinary, Date, Time, DateTime
  {
    OGRFieldDefn f("f_bin", OFTBinary);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_date", OFTDate);
    f.SetDefault("'2026-01-01'");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_time", OFTTime);
    f.SetDefault("'12:34:56'");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }
  {
    OGRFieldDefn f("f_datetime", OFTDateTime);
    f.SetDefault("CURRENT_TIMESTAMP");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f));
  }

  // Insert a test feature
  OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
  feat->SetField("f_int", 1);
  feat->SetField("f_bool", 1);
  feat->SetField("f_int16", 32000);
  feat->SetField("f_int64", static_cast<GIntBig>(9876543210LL));
  feat->SetField("f_real", 45.67);
  feat->SetField("f_float32", 12.5);
  feat->SetField("f_str", "user@google.com");
  feat->SetField("f_json", "{\"key\":\"val\"}");
  feat->SetField("f_uuid", "123e4567-e89b-12d3-a456-426614174000");
  GByte raw_bytes[] = {0x01, 0x02, 0x03, 0x04};
  feat->SetField(feat->GetFieldIndex("f_bin"), 4, raw_bytes);
  feat->SetField("f_date", "2026-11-03");
  feat->SetField("f_time", "10:30:00");
  feat->SetField("f_datetime", "2026-11-03T10:30:00Z");

  OGRPoint pt(10.0, 20.0);
  feat->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  // Close and reopen dataset to thoroughly test ReadTableDefinition()
  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, dataset_);

  layer = dataset_->GetLayerByName("all_types");
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(1, layer->GetFeatureCount());

  OGRFeature* read_feat = layer->GetNextFeature();
  ASSERT_NE(nullptr, read_feat);
  EXPECT_EQ(1, read_feat->GetFieldAsInteger("f_int"));
  EXPECT_EQ(1, read_feat->GetFieldAsInteger("f_bool"));
  EXPECT_EQ(32000, read_feat->GetFieldAsInteger("f_int16"));
  EXPECT_EQ(9876543210LL, read_feat->GetFieldAsInteger64("f_int64"));
  EXPECT_NEAR(45.67, read_feat->GetFieldAsDouble("f_real"), 1e-4);
  EXPECT_STREQ("user@google.com", read_feat->GetFieldAsString("f_str"));
  OGRFeature::DestroyFeature(read_feat);
}

// Tests AlterFieldDefn, DeleteField, ReorderFields, and AlterGeomFieldDefn.
TEST_F(OGRGeoPackageTableLayerTest, SchemaAlterationFieldAndGeom) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      dataset_->CreateLayer("schema_alter", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f1("col_a", OFTInteger);
  OGRFieldDefn f2("col_b", OFTString);
  f2.SetWidth(50);
  OGRFieldDefn f3("col_c", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f1));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f2));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f3));

  // Insert 2 features so operations requiring table recreation work with data
  for (int i = 1; i <= 2; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("col_a", i * 10);
    feat->SetField("col_b", absl::StrCat("text_", i).c_str());
    feat->SetField("col_c", i * 1.5);
    OGRPoint pt(i, i);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 1. Rename column (in-place)
  {
    int idx = layer->GetLayerDefn()->GetFieldIndex("col_a");
    ASSERT_GE(idx, 0);
    OGRFieldDefn new_f("col_a_renamed", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, layer->AlterFieldDefn(idx, &new_f, ALTER_NAME_FLAG));
    EXPECT_GE(layer->GetLayerDefn()->GetFieldIndex("col_a_renamed"), 0);
  }

  // 2. Alter column type and width (triggers table recreation)
  {
    int idx = layer->GetLayerDefn()->GetFieldIndex("col_b");
    ASSERT_GE(idx, 0);
    OGRFieldDefn new_f("col_b", OFTString);
    new_f.SetWidth(100);
    new_f.SetNullable(false);
    new_f.SetDefault("'default_text'");
    EXPECT_EQ(OGRERR_NONE, layer->AlterFieldDefn(idx, &new_f,
                                                 ALTER_WIDTH_PRECISION_FLAG |
                                                     ALTER_NULLABLE_FLAG |
                                                     ALTER_DEFAULT_FLAG));
  }

  // 3. Reorder fields
  {
    int map[] = {2, 0, 1};
    EXPECT_EQ(OGRERR_NONE, layer->ReorderFields(map));
  }

  // 4. Delete field
  {
    int idx = layer->GetLayerDefn()->GetFieldIndex("col_c");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(OGRERR_NONE, layer->DeleteField(idx));
    EXPECT_LT(layer->GetLayerDefn()->GetFieldIndex("col_c"), 0);
  }

  // 5. AlterGeomFieldDefn
  {
    OGRGeomFieldDefn new_geom("geom_renamed", wkbPoint);
    auto* srs_3857 = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, srs_3857->importFromEPSG(3857));
    srs_3857->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    new_geom.SetSpatialRef(srs_3857);
    srs_3857->Release();

    EXPECT_EQ(OGRERR_NONE,
              layer->AlterGeomFieldDefn(0, &new_geom,
                                        ALTER_GEOM_FIELD_DEFN_NAME_FLAG |
                                            ALTER_GEOM_FIELD_DEFN_SRS_FLAG));
    EXPECT_STREQ("geom_renamed", layer->GetGeometryColumn());

    // Altering type or nullable is not supported in GPKG and returns failure
    OGRGeomFieldDefn invalid_type("geom_renamed", wkbPoint25D);
    EXPECT_EQ(OGRERR_FAILURE,
              layer->AlterGeomFieldDefn(0, &invalid_type,
                                        ALTER_GEOM_FIELD_DEFN_TYPE_FLAG));
  }

  // Verify feature count remains 2 after all schema manipulations
  EXPECT_EQ(2, layer->GetFeatureCount());
}

// Tests feature CRUD operations: Create, Read, Set, Upsert, Update, Delete,
// SetNextByIndex.
TEST_F(OGRGeoPackageTableLayerTest, FeatureCRUDUpsertUpdateAndBind) {
  OGRLayer* layer =
      dataset_->CreateLayer("crud_test", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_name("name", OFTString);
  OGRFieldDefn f_val("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_name));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_val));

  // 1. CreateFeature with auto-assigned FID
  OGRFeature* feat1 = OGRFeature::CreateFeature(layer->GetLayerDefn());
  feat1->SetField("name", "first");
  feat1->SetField("val", 100);
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(10, 0);
  ring.addPoint(10, 10);
  ring.addPoint(0, 10);
  ring.addPoint(0, 0);
  poly.addRing(&ring);
  feat1->SetGeometry(&poly);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat1));
  const GIntBig fid1 = feat1->GetFID();
  EXPECT_GT(fid1, 0);
  OGRFeature::DestroyFeature(feat1);

  // 2. CreateFeature with explicit FID
  OGRFeature* feat2 = OGRFeature::CreateFeature(layer->GetLayerDefn());
  feat2->SetFID(99);
  feat2->SetField("name", "explicit_fid");
  feat2->SetField("val", 200);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat2));
  EXPECT_EQ(99, feat2->GetFID());
  OGRFeature::DestroyFeature(feat2);

  // 3. GetFeature
  OGRFeature* read_feat = layer->GetFeature(fid1);
  ASSERT_NE(nullptr, read_feat);
  EXPECT_STREQ("first", read_feat->GetFieldAsString("name"));
  EXPECT_EQ(100, read_feat->GetFieldAsInteger("val"));

  // 4. SetFeature (update)
  read_feat->SetField("name", "first_updated");
  EXPECT_EQ(OGRERR_NONE, layer->SetFeature(read_feat));
  OGRFeature::DestroyFeature(read_feat);

  // 5. IUpsertFeature
  // 5a. Upsert on existing feature
  OGRFeature* upsert_exist = OGRFeature::CreateFeature(layer->GetLayerDefn());
  upsert_exist->SetFID(fid1);
  upsert_exist->SetField("name", "upserted_existing");
  upsert_exist->SetField("val", 150);
  EXPECT_EQ(OGRERR_NONE, layer->UpsertFeature(upsert_exist));
  OGRFeature::DestroyFeature(upsert_exist);

  // 5b. Upsert on new feature
  OGRFeature* upsert_new = OGRFeature::CreateFeature(layer->GetLayerDefn());
  upsert_new->SetFID(250);
  upsert_new->SetField("name", "upserted_new");
  upsert_new->SetField("val", 300);
  EXPECT_EQ(OGRERR_NONE, layer->UpsertFeature(upsert_new));
  OGRFeature::DestroyFeature(upsert_new);

  // 6. IUpdateFeature (partial update)
  OGRFeature* partial = OGRFeature::CreateFeature(layer->GetLayerDefn());
  partial->SetFID(fid1);
  partial->SetField("val", 999);
  int updated_fields[] = {1};  // update only "val"
  EXPECT_EQ(OGRERR_NONE, layer->UpdateFeature(partial, 1, updated_fields, 0,
                                              nullptr, false));
  OGRFeature::DestroyFeature(partial);

  // 7. SetNextByIndex
  EXPECT_EQ(OGRERR_NONE, layer->SetNextByIndex(1));
  OGRFeature* by_index = layer->GetNextFeature();
  ASSERT_NE(nullptr, by_index);
  OGRFeature::DestroyFeature(by_index);

  // 8. DeleteFeature
  EXPECT_EQ(OGRERR_NONE, layer->DeleteFeature(99));
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, layer->DeleteFeature(99));

  // 9. Attribute Filter
  layer->SetAttributeFilter("val = 999");
  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->SetAttributeFilter(nullptr);
}

// Tests RTree spatial indexing, spatial queries, and deferred/threaded index
// creation.
TEST_F(OGRGeoPackageTableLayerTest, SpatialIndexRTreeDeferredAndThreaded) {
  OGRLayer* layer =
      dataset_->CreateLayer("spatial_idx", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
  ASSERT_NE(nullptr, gpkg_layer);

  // Add 10 points with distinct coordinates
  for (int i = 0; i < 10; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint pt(i * 10.0, i * 10.0);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  EXPECT_TRUE(gpkg_layer->HasSpatialIndex());

  // Spatial filter with envelope
  layer->SetSpatialFilterRect(5.0, 5.0, 25.0, 25.0);
  EXPECT_EQ(2, layer->GetFeatureCount());  // (10,10) and (20,20)
  layer->SetSpatialFilter(nullptr);

  // Drop and recreate spatial index
  EXPECT_TRUE(gpkg_layer->DropSpatialIndex());
  EXPECT_FALSE(gpkg_layer->HasSpatialIndex());
  EXPECT_TRUE(gpkg_layer->CreateSpatialIndex());
  EXPECT_TRUE(gpkg_layer->HasSpatialIndex());

  // Test deferred spatial index update inside transaction
  CPLSetThreadLocalConfigOption("OGR_GPKG_DEFERRED_SPI_UPDATE_THRESHOLD", "2");
  ASSERT_EQ(OGRERR_NONE, layer->StartTransaction());
  for (int i = 10; i < 15; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint pt(i * 10.0, i * 10.0);
    feat->SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  ASSERT_EQ(OGRERR_NONE, layer->CommitTransaction());
  CPLSetThreadLocalConfigOption("OGR_GPKG_DEFERRED_SPI_UPDATE_THRESHOLD",
                                nullptr);

  // Test threaded async RTree
  const std::string threaded_path = "/vsimem/threaded_rtree.gpkg";
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);
  {
    CPLSetThreadLocalConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE",
                                  "YES");
    CPLSetThreadLocalConfigOption("OGR_GPKG_MAX_RAM_USAGE_RTREE", "100");

    std::unique_ptr<GDALDataset> threaded_ds(
        driver->Create(threaded_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (threaded_ds != nullptr) {
      OGRLayer* t_layer =
          threaded_ds->CreateLayer("t_threaded", nullptr, wkbPoint, nullptr);
      if (t_layer != nullptr) {
        for (int i = 0; i < 25; ++i) {
          OGRFeature* feat = OGRFeature::CreateFeature(t_layer->GetLayerDefn());
          OGRPoint pt(i, i);
          feat->SetGeometry(&pt);
          EXPECT_EQ(OGRERR_NONE, t_layer->CreateFeature(feat));
          OGRFeature::DestroyFeature(feat);
        }
        t_layer->SyncToDisk();
      }
    }

    CPLSetThreadLocalConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE",
                                  nullptr);
    CPLSetThreadLocalConfigOption("OGR_GPKG_MAX_RAM_USAGE_RTREE", nullptr);
    VSIUnlink(threaded_path.c_str());
  }
}

// Tests 2D/3D extents, geometry types inspection, and non-standard geometries.
TEST_F(OGRGeoPackageTableLayerTest, Extents3DGeometryTypesAndExtensions) {
  OGRLayer* layer =
      dataset_->CreateLayer("extents_3d", nullptr, wkbPointZM, nullptr);
  ASSERT_NE(nullptr, layer);

  // Insert 3D points with Z and M
  {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint pt(0.0, 0.0, 10.0, 1.0);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint pt(100.0, 50.0, 40.0, 2.0);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 2D Extent
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env, true));
  EXPECT_NEAR(0.0, env.MinX, 1e-4);
  EXPECT_NEAR(100.0, env.MaxX, 1e-4);
  EXPECT_NEAR(0.0, env.MinY, 1e-4);
  EXPECT_NEAR(50.0, env.MaxY, 1e-4);

  // 3D Extent
  OGREnvelope3D env3d;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent3D(0, &env3d, true));
  EXPECT_NEAR(10.0, env3d.MinZ, 1e-4);
  EXPECT_NEAR(40.0, env3d.MaxZ, 1e-4);

  // Geometry Types Inspection
  int entry_count = 0;
  auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
  ASSERT_NE(nullptr, gpkg_layer);
  OGRGeometryTypeCounter* counters =
      gpkg_layer->GetGeometryTypes(0, 0, entry_count, nullptr, nullptr);
  EXPECT_NE(nullptr, counters);
  EXPECT_GT(entry_count, 0);
  CPLFree(counters);

  // Non-linear curve geometry extensions (CircularString)
  OGRLayer* curve_layer =
      dataset_->CreateLayer("curves", nullptr, wkbCircularString, nullptr);
  ASSERT_NE(nullptr, curve_layer);

  OGRFeature* c_feat = OGRFeature::CreateFeature(curve_layer->GetLayerDefn());
  OGRCircularString cs;
  cs.addPoint(0, 0);
  cs.addPoint(1, 1);
  cs.addPoint(2, 0);
  c_feat->SetGeometry(&cs);
  EXPECT_EQ(OGRERR_NONE, curve_layer->CreateFeature(c_feat));
  OGRFeature::DestroyFeature(c_feat);
}

// Tests Arrow streaming across all field types, both synchronous and
// asynchronous.
TEST_F(OGRGeoPackageTableLayerTest, ArrowStreamSyncAndAsync) {
  OGRLayer* layer =
      dataset_->CreateLayer("arrow_test", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_int("f_int", OFTInteger);
  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  OGRFieldDefn f_real("f_real", OFTReal);
  OGRFieldDefn f_str("f_str", OFTString);
  OGRFieldDefn f_bin("f_bin", OFTBinary);
  OGRFieldDefn f_dt("f_dt", OFTDateTime);

  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bin));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  for (int i = 0; i < 10; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    if (i % 2 == 0) {
      feat->SetField("f_int", i * 10);
      feat->SetField("f_int64", static_cast<GIntBig>(i * 1000000000LL));
      feat->SetField("f_real", i * 1.25);
      feat->SetField("f_str", absl::StrCat("str_", i).c_str());
      GByte b[] = {static_cast<GByte>(i), 0xFF};
      feat->SetField(feat->GetFieldIndex("f_bin"), 2, b);
      feat->SetField("f_dt", "2026-11-03T10:00:00Z");
      OGRPoint pt(i, i);
      feat->SetGeometry(&pt);
    }
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 1. Synchronous Arrow streaming
  {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer->GetArrowStream(&stream, nullptr));
    ASSERT_NE(nullptr, stream.get_next);

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(10, array.length);
    if (array.release) {
      array.release(&array);
    }

    if (stream.release) {
      stream.release(&stream);
    }
  }

  // 2. Asynchronous Arrow streaming (with attribute filter and batch size)
  {
    layer->ResetReading();
    layer->SetAttributeFilter("f_int >= 0");

    const char* const opts[] = {"MAX_FEATURES_IN_BATCH=3", nullptr};
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer->GetArrowStream(&stream, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, stream.get_next);

    int total_read = 0;
    while (true) {
      struct ArrowArray array;
      memset(&array, 0, sizeof(array));
      EXPECT_EQ(0, stream.get_next(&stream, &array));
      if (array.length == 0) {
        if (array.release) array.release(&array);
        break;
      }
      total_read += array.length;
      if (array.release) {
        array.release(&array);
      }
    }
    EXPECT_EQ(5, total_read);  // 5 non-null rows

    if (stream.release) {
      stream.release(&stream);
    }
    layer->SetAttributeFilter(nullptr);
  }

  // 3. Early cancel of asynchronous Arrow stream
  {
    layer->ResetReading();
    layer->SetAttributeFilter("f_int >= 0");
    const char* const opts[] = {"MAX_FEATURES_IN_BATCH=2", nullptr};
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer->GetArrowStream(&stream, const_cast<char**>(opts)));

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    if (array.release) array.release(&array);

    // Cancel without reading all batches
    if (stream.release) {
      stream.release(&stream);
    }
    layer->SetAttributeFilter(nullptr);
  }
}

// Tests Views, Rename, and Truncate.
TEST_F(OGRGeoPackageTableLayerTest, ViewsRenameAndTruncate) {
  OGRLayer* layer =
      dataset_->CreateLayer("tbl_rename", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f("col_val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f));

  for (int i = 0; i < 5; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("col_val", i);
    OGRPoint pt(i, i);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 1. Rename table
  EXPECT_EQ(OGRERR_NONE, layer->Rename("tbl_renamed_dest"));
  EXPECT_STREQ("tbl_renamed_dest", layer->GetName());
  EXPECT_NE(nullptr, dataset_->GetLayerByName("tbl_renamed_dest"));

  // 2. Truncate table
  EXPECT_EQ(5, layer->GetFeatureCount());
  auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
  ASSERT_NE(nullptr, gpkg_layer);
  EXPECT_EQ(OGRERR_NONE, gpkg_layer->Truncate());
  EXPECT_EQ(0, layer->GetFeatureCount());

  // 3. Create view and test view layer
  dataset_->ExecuteSQL(
      "CREATE VIEW v_pts AS SELECT tbl_renamed_dest.fid AS OGR_STYLE, "
      "tbl_renamed_dest.* FROM tbl_renamed_dest",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_contents (table_name, data_type, identifier) "
      "VALUES ('v_pts', 'features', 'v_pts')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_geometry_columns (table_name, column_name, "
      "geometry_type_name, srs_id, z, m) "
      "VALUES ('v_pts', 'geom', 'POINT', 0, 0, 0)",
      nullptr, nullptr);

  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, dataset_);

  OGRLayer* view_layer = dataset_->GetLayerByName("v_pts");
  ASSERT_NE(nullptr, view_layer);
  EXPECT_FALSE(view_layer->TestCapability(OLCRandomWrite));
}

// Tests multithreaded Arrow array prefetching and thread cancellation.
TEST_F(OGRGeoPackageTableLayerTest, MultithreadedArrowArrayPrefetch) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      dataset_->CreateLayer("tbl_prefetch", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_val("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_val));

  // Populate 50 features with contiguous FIDs (1..50)
  for (int i = 1; i <= 50; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetFID(i);
    feat->SetField("val", i * 10);
    OGRPoint pt(i * 0.1, i * 0.1);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Close write dataset
  dataset_.reset();

  // Reopen in read-only mode to enable prefetch worker threads
  dataset_.reset(
      GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, dataset_);

  OGRLayer* ro_layer = dataset_->GetLayerByName("tbl_prefetch");
  ASSERT_NE(nullptr, ro_layer);

  // Set OGR_GPKG_NUM_THREADS = 2 to allow background prefetch threads
  CPLSetThreadLocalConfigOption("OGR_GPKG_NUM_THREADS", "2");
  auto cleanup_threads = absl::MakeCleanup(
      []() { CPLSetThreadLocalConfigOption("OGR_GPKG_NUM_THREADS", nullptr); });

  // 1. Read all batches with batch size 5
  {
    const char* const opts[] = {"MAX_FEATURES_IN_BATCH=5", nullptr};
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT_TRUE(ro_layer->GetArrowStream(&stream, const_cast<char**>(opts)));

    int total_features = 0;
    while (true) {
      struct ArrowArray array;
      memset(&array, 0, sizeof(array));
      int res = stream.get_next(&stream, &array);
      EXPECT_EQ(0, res);
      if (array.release == nullptr) break;
      total_features += array.length;
      array.release(&array);
    }
    EXPECT_EQ(50, total_features);
    if (stream.release) stream.release(&stream);
  }

  // 2. Early cancellation of prefetching
  {
    ro_layer->ResetReading();
    const char* const opts[] = {"MAX_FEATURES_IN_BATCH=5", nullptr};
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT_TRUE(ro_layer->GetArrowStream(&stream, const_cast<char**>(opts)));

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    if (array.release) array.release(&array);

    // Cancel before reading remaining batches
    if (stream.release) stream.release(&stream);
  }

  // Reopen update dataset for subsequent cleanup in TearDown
  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
}

// Tests metadata persistence, custom domains, and 64-bit FID detection.
TEST_F(OGRGeoPackageTableLayerTest, TableMetadataStorageAndDomainQueries) {
  OGRLayer* layer =
      dataset_->CreateLayer("tbl_metadata", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f("col", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f));

  // Insert feature with FID > INT_MAX to trigger 64-bit FID detection
  OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
  feat->SetFID(5000000000LL);
  feat->SetField("col", 42);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  // Set layer metadata in default and custom domains
  EXPECT_EQ(CE_None, layer->SetMetadataItem("CUSTOM_KEY", "custom_val"));
  char** custom_md = CSLAddNameValue(nullptr, "KEY_A", "VAL_A");
  custom_md = CSLAddNameValue(custom_md, "KEY_B", "VAL_B");
  EXPECT_EQ(CE_None, layer->SetMetadata(custom_md, "MY_DOMAIN"));
  CSLDestroy(custom_md);

  // Also insert non-GDAL metadata directly into gpkg_metadata and
  // gpkg_metadata_reference
  dataset_->ExecuteSQL(
      "CREATE TABLE IF NOT EXISTS gpkg_metadata ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "md_scope TEXT NOT NULL DEFAULT 'dataset',"
      "md_standard_uri TEXT NOT NULL,"
      "mime_type TEXT NOT NULL DEFAULT 'text/xml',"
      "metadata TEXT NOT NULL DEFAULT '')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "CREATE TABLE IF NOT EXISTS gpkg_metadata_reference ("
      "reference_scope TEXT NOT NULL,"
      "table_name TEXT,"
      "column_name TEXT,"
      "row_id_value INTEGER,"
      "timestamp DATETIME NOT NULL DEFAULT "
      "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
      "md_file_id INTEGER NOT NULL,"
      "md_parent_id INTEGER,"
      "CONSTRAINT r_scope_check CHECK (reference_scope IN "
      "('geopackage','table','column','row/col','series','dataset','feature','"
      "attribute')),"
      "CONSTRAINT fk_rmfi_md_file_id FOREIGN KEY (md_file_id) REFERENCES "
      "gpkg_metadata(id),"
      "CONSTRAINT fk_rmfi_md_parent_id FOREIGN KEY (md_parent_id) REFERENCES "
      "gpkg_metadata(id))",
      nullptr, nullptr);

  // Non-GDAL text/xml metadata record referencing tbl_metadata
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_metadata (id, md_scope, md_standard_uri, mime_type, "
      "metadata) "
      "VALUES (101, 'table', 'http://custom.org', 'text/plain', 'Custom plain "
      "text metadata')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_metadata_reference (reference_scope, table_name, "
      "md_file_id) "
      "VALUES ('table', 'tbl_metadata', 101)",
      nullptr, nullptr);

  // Reopen dataset
  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, dataset_);

  layer = dataset_->GetLayerByName("tbl_metadata");
  ASSERT_NE(nullptr, layer);

  // Query metadata items and domain list
  char** domains = layer->GetMetadataDomainList();
  EXPECT_NE(nullptr, domains);
  CSLDestroy(domains);

  EXPECT_STREQ("YES", layer->GetMetadataItem(OLMD_FID64));
  const char* val = layer->GetMetadataItem("CUSTOM_KEY");
  if (val) {
    EXPECT_STREQ("custom_val", val);
  }
  const char* non_gdal = layer->GetMetadataItem("GPKG_METADATA_ITEM_1");
  EXPECT_NE(nullptr, non_gdal);

  char** my_domain_md = layer->GetMetadata("MY_DOMAIN");
  if (my_domain_md) {
    EXPECT_STREQ("VAL_A", CSLFetchNameValue(my_domain_md, "KEY_A"));
  }
}

// Tests RTree-based GetExtent and UpdateContentsToNullExtent.
TEST_F(OGRGeoPackageTableLayerTest, GetExtentFromRTreeAndNullExtent) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("tbl_rtree_ext", &srs, wkbPoint,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  for (int i = 1; i <= 10; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint pt(10.0 + i, 20.0 + i);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Clear gpkg_contents extents to force calculating from RTree
  dataset_->ExecuteSQL(
      "UPDATE gpkg_contents SET min_x=null, min_y=null, max_x=null, "
      "max_y=null WHERE table_name='tbl_rtree_ext'",
      nullptr, nullptr);

  // Close and reopen to ensure RTree is flushed and indexed
  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, dataset_);

  layer = dataset_->GetLayerByName("tbl_rtree_ext");
  ASSERT_NE(nullptr, layer);

  // 1. Force RTree extent reading
  {
    CPLSetThreadLocalConfigOption("OGR_GPKG_USE_RTREE_FOR_GET_EXTENT", "YES");
    auto cleanup_rtree = absl::MakeCleanup([]() {
      CPLSetThreadLocalConfigOption("OGR_GPKG_USE_RTREE_FOR_GET_EXTENT",
                                    nullptr);
    });

    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env, false));
    EXPECT_NEAR(11.0, env.MinX, 1e-4);
    EXPECT_NEAR(20.0, env.MaxX, 1e-4);
    EXPECT_NEAR(21.0, env.MinY, 1e-4);
    EXPECT_NEAR(30.0, env.MaxY, 1e-4);
  }

  // 2. Force SQL-based extent calculation when RTree extent is disabled
  {
    dataset_->ExecuteSQL(
        "UPDATE gpkg_contents SET min_x=null, min_y=null, max_x=null, "
        "max_y=null WHERE table_name='tbl_rtree_ext'",
        nullptr, nullptr);
    dataset_.reset();
    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, dataset_);
    layer = dataset_->GetLayerByName("tbl_rtree_ext");
    ASSERT_NE(nullptr, layer);

    CPLSetThreadLocalConfigOption("OGR_GPKG_USE_RTREE_FOR_GET_EXTENT", "NO");
    auto cleanup_rtree = absl::MakeCleanup([]() {
      CPLSetThreadLocalConfigOption("OGR_GPKG_USE_RTREE_FOR_GET_EXTENT",
                                    nullptr);
    });

    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env, true));
    EXPECT_NEAR(11.0, env.MinX, 1e-4);
    EXPECT_NEAR(20.0, env.MaxX, 1e-4);
    EXPECT_NEAR(21.0, env.MinY, 1e-4);
    EXPECT_NEAR(30.0, env.MaxY, 1e-4);
  }

  // 3. Broken RTree detection heuristic
  {
    dataset_->ExecuteSQL("DELETE FROM rtree_tbl_rtree_ext_geom WHERE id = 10",
                         nullptr, nullptr);
    dataset_.reset();

    CPLSetThreadLocalConfigOption("OGR_GPKG_THRESHOLD_DETECT_BROKEN_RTREE",
                                  "1");
    auto cleanup_thresh = absl::MakeCleanup([]() {
      CPLSetThreadLocalConfigOption("OGR_GPKG_THRESHOLD_DETECT_BROKEN_RTREE",
                                    nullptr);
    });

    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, dataset_);
    layer = dataset_->GetLayerByName("tbl_rtree_ext");
    ASSERT_NE(nullptr, layer);

    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_FALSE(layer->TestCapability(OLCFastSpatialFilter));
  }

  // 4. Calling GetExtent on an empty layer triggers
  // UpdateContentsToNullExtent()
  OGREnvelope env;
  OGRLayer* empty_layer =
      dataset_->CreateLayer("tbl_empty_ext", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, empty_layer);
  EXPECT_EQ(OGRERR_FAILURE, empty_layer->GetExtent(&env, true));
}

// Tests AlterFieldDefn, DeleteField, ReorderFields, and AlterGeomFieldDefn.
TEST_F(OGRGeoPackageTableLayerTest, AlterFieldDefnDeleteAndReorder) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("tbl_alter", &srs, wkbPoint,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f1("col_int", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f1));
  OGRFieldDefn f2("col_str", OFTString);
  f2.SetWidth(50);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f2));
  OGRFieldDefn f3("col_real", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f3));

  for (int i = 1; i <= 3; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("col_int", i * 10);
    feat->SetField("col_str", absl::StrCat("text_", i).c_str());
    feat->SetField("col_real", i * 1.5);
    OGRPoint pt(i, i);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // 1. Error cases for AlterFieldDefn
  {
    OGRFieldDefn bad("bad", OFTInteger);
    EXPECT_EQ(OGRERR_FAILURE, layer->AlterFieldDefn(-1, &bad, ALTER_ALL_FLAG));
    EXPECT_EQ(OGRERR_FAILURE, layer->AlterFieldDefn(99, &bad, ALTER_ALL_FLAG));
    bad.SetName("col_str");
    EXPECT_EQ(OGRERR_FAILURE, layer->AlterFieldDefn(0, &bad, ALTER_NAME_FLAG));
  }

  // 2. Alter column types and flags (Integer -> Real -> String)
  {
    OGRFieldDefn new_f("col_int_to_real", OFTReal);
    new_f.SetDefault("3.14");
    new_f.SetAlternativeName("alt_col_real");
    new_f.SetComment("A comment on real col");
    EXPECT_EQ(OGRERR_NONE,
              layer->AlterFieldDefn(
                  0, &new_f,
                  ALTER_NAME_FLAG | ALTER_TYPE_FLAG | ALTER_DEFAULT_FLAG |
                      ALTER_ALTERNATIVE_NAME_FLAG | ALTER_COMMENT_FLAG));
    EXPECT_STREQ("col_int_to_real",
                 layer->GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
    EXPECT_EQ(OFTReal, layer->GetLayerDefn()->GetFieldDefn(0)->GetType());
  }

  // 3. Alter width and precision
  {
    OGRFieldDefn new_str("col_str_resized", OFTString);
    new_str.SetWidth(100);
    new_str.SetPrecision(0);
    new_str.SetNullable(false);
    EXPECT_EQ(OGRERR_NONE, layer->AlterFieldDefn(
                               1, &new_str,
                               ALTER_NAME_FLAG | ALTER_WIDTH_PRECISION_FLAG |
                                   ALTER_NULLABLE_FLAG));
    EXPECT_FALSE(layer->GetLayerDefn()->GetFieldDefn(1)->IsNullable());
  }

  // 4. Alter unique constraint
  {
    OGRFieldDefn new_unique("col_str_resized", OFTString);
    new_unique.SetUnique(true);
    EXPECT_EQ(OGRERR_NONE,
              layer->AlterFieldDefn(1, &new_unique, ALTER_UNIQUE_FLAG));
  }

  // 5. Reorder fields
  {
    int map[3] = {2, 0, 1};
    EXPECT_EQ(OGRERR_NONE, layer->ReorderFields(map));
  }

  // 6. Delete field
  {
    EXPECT_EQ(OGRERR_FAILURE, layer->DeleteField(-1));
    EXPECT_EQ(OGRERR_FAILURE, layer->DeleteField(100));
    EXPECT_EQ(OGRERR_NONE, layer->DeleteField(0));
    EXPECT_EQ(2, layer->GetLayerDefn()->GetFieldCount());
  }

  // 7. AlterGeomFieldDefn (rename geometry column with spatial index, and test
  // unsupported flags)
  {
    auto* srs = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, srs->importFromEPSG(3857));
    OGRGeomFieldDefn new_geom("geom_renamed", wkbPoint);
    new_geom.SetSpatialRef(srs);
    srs->Release();

    // Unsupported geometry type alteration
    OGRGeomFieldDefn type_alter("geom_renamed", wkbLineString);
    EXPECT_EQ(OGRERR_FAILURE,
              layer->AlterGeomFieldDefn(0, &type_alter,
                                        ALTER_GEOM_FIELD_DEFN_TYPE_FLAG));

    // Unsupported nullable alteration
    OGRGeomFieldDefn null_alter("geom_renamed", wkbPoint);
    null_alter.SetNullable(false);
    EXPECT_EQ(OGRERR_FAILURE,
              layer->AlterGeomFieldDefn(0, &null_alter,
                                        ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG));

    // Valid rename with spatial index
    EXPECT_EQ(OGRERR_NONE,
              layer->AlterGeomFieldDefn(0, &new_geom,
                                        ALTER_GEOM_FIELD_DEFN_NAME_FLAG |
                                            ALTER_GEOM_FIELD_DEFN_SRS_FLAG));
    EXPECT_STREQ("geom_renamed",
                 layer->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef());
  }
}

// Tests geometry type validation and non-standard geometry extensions.
TEST_F(OGRGeoPackageTableLayerTest, CheckGeometryTypeAndExtensions) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      dataset_->CreateLayer("tbl_geom_ext", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  // 1. Mismatched geometry type warning/error
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    feat->SetGeometry(&ls);
    // May succeed with warning or fail depending on settings; verify it runs
    // through CheckGeometryType
    (void)layer->CreateFeature(feat);
    OGRFeature::DestroyFeature(feat);
  }

  // 2. 2.5D geometry in 2D layer
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint pt3d(1.0, 2.0, 3.0);
    feat->SetGeometry(&pt3d);
    (void)layer->CreateFeature(feat);
    OGRFeature::DestroyFeature(feat);
  }

  // 3. Curve / non-standard geometry types requiring extensions
  OGRLayer* curve_layer =
      dataset_->CreateLayer("tbl_curves", &srs, wkbCircularString, nullptr);
  if (curve_layer) {
    OGRFeature* feat = OGRFeature::CreateFeature(curve_layer->GetLayerDefn());
    OGRCircularString cs;
    cs.addPoint(0, 0);
    cs.addPoint(1, 1);
    cs.addPoint(2, 0);
    feat->SetGeometry(&cs);
    (void)curve_layer->CreateFeature(feat);
    OGRFeature::DestroyFeature(feat);
  }

  OGRLayer* comp_layer =
      dataset_->CreateLayer("tbl_comp", &srs, wkbCompoundCurve, nullptr);
  if (comp_layer) {
    OGRFeature* feat = OGRFeature::CreateFeature(comp_layer->GetLayerDefn());
    OGRCompoundCurve cc;
    auto* ls = new OGRLineString();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    cc.addCurveDirectly(ls);
    feat->SetGeometry(&cc);
    (void)comp_layer->CreateFeature(feat);
    OGRFeature::DestroyFeature(feat);
  }

  OGRLayer* poly_layer =
      dataset_->CreateLayer("tbl_curve_poly", &srs, wkbCurvePolygon, nullptr);
  if (poly_layer) {
    OGRFeature* feat = OGRFeature::CreateFeature(poly_layer->GetLayerDefn());
    OGRCurvePolygon cp;
    auto* ring = new OGRCircularString();
    ring->addPoint(0, 0);
    ring->addPoint(2, 0);
    ring->addPoint(2, 2);
    ring->addPoint(0, 2);
    ring->addPoint(0, 0);
    cp.addRingDirectly(ring);
    feat->SetGeometry(&cp);
    (void)poly_layer->CreateFeature(feat);
    OGRFeature::DestroyFeature(feat);
  }

  // 4. Register an extension in gpkg_extensions and reopen
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('tbl_geom_ext', 'geom', 'gpkg_geom_POINT', "
      "'http://example.com', 'read-write')",
      nullptr, nullptr);

  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, dataset_);
  EXPECT_NE(nullptr, dataset_->GetLayerByName("tbl_geom_ext"));
}

// Tests Rename and Truncate on tables with spatial indices, triggers, and
// metadata.
TEST_F(OGRGeoPackageTableLayerTest, RenameAndTruncateComplex) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("tbl_complex_rename", &srs, wkbPoint,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f));

  for (int i = 1; i <= 5; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("val", i);
    OGRPoint pt(i, i);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Attach metadata
  EXPECT_EQ(CE_None, layer->SetMetadataItem("LAYER_MD", "value"));

  // Create another table to test rename collision
  OGRLayer* other =
      dataset_->CreateLayer("tbl_existing", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, other);
  EXPECT_EQ(OGRERR_NONE, other->SyncToDisk());

  // 1. Rename collision should fail
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_EQ(OGRERR_FAILURE, layer->Rename("tbl_existing"));
  }

  // 2. Successful rename
  EXPECT_EQ(OGRERR_NONE, layer->Rename("tbl_complex_renamed"));
  EXPECT_STREQ("tbl_complex_renamed", layer->GetName());
  EXPECT_NE(nullptr, dataset_->GetLayerByName("tbl_complex_renamed"));
  EXPECT_EQ(nullptr, dataset_->GetLayerByName("tbl_complex_rename"));

  // 3. Truncate
  auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
  ASSERT_NE(nullptr, gpkg_layer);
  EXPECT_EQ(5, layer->GetFeatureCount());
  EXPECT_EQ(OGRERR_NONE, gpkg_layer->Truncate());
  EXPECT_EQ(0, layer->GetFeatureCount());
}

// Tests SetFeature, UpsertFeature, and RTree triggers.
TEST_F(OGRGeoPackageTableLayerTest, WorkaroundUpdate1TriggerIssueAndUpsert) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("tbl_update_trig", &srs, wkbPoint,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f("name", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f));

  // Insert feature
  OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
  feat->SetFID(1);
  feat->SetField("name", "initial");
  OGRPoint pt(10.0, 20.0);
  feat->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // 1. SetFeature updating both geometry and attributes
  {
    OGRFeature* update_feat = layer->GetFeature(1);
    ASSERT_NE(nullptr, update_feat);
    update_feat->SetField("name", "updated");
    OGRPoint new_pt(15.0, 25.0);
    update_feat->SetGeometry(&new_pt);
    EXPECT_EQ(OGRERR_NONE, layer->SetFeature(update_feat));
    OGRFeature::DestroyFeature(update_feat);
  }

  // 2. SetFeature updating only attributes (geometry unchanged)
  {
    OGRFeature* update_feat = layer->GetFeature(1);
    ASSERT_NE(nullptr, update_feat);
    update_feat->SetField("name", "attr_only");
    EXPECT_EQ(OGRERR_NONE, layer->SetFeature(update_feat));
    OGRFeature::DestroyFeature(update_feat);
  }

  // 3. SetFeature setting null geometry
  {
    OGRFeature* update_feat = layer->GetFeature(1);
    ASSERT_NE(nullptr, update_feat);
    update_feat->SetGeometry(nullptr);
    EXPECT_EQ(OGRERR_NONE, layer->SetFeature(update_feat));
    OGRFeature::DestroyFeature(update_feat);
  }

  // 4. UpsertFeature
  {
    OGRFeature* upsert_feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    upsert_feat->SetFID(1);
    upsert_feat->SetField("name", "upserted");
    OGRPoint pt2(30.0, 40.0);
    upsert_feat->SetGeometry(&pt2);
    EXPECT_EQ(OGRERR_NONE, layer->UpsertFeature(upsert_feat));
    OGRFeature::DestroyFeature(upsert_feat);

    // Upsert a brand new feature
    OGRFeature* upsert_new = OGRFeature::CreateFeature(layer->GetLayerDefn());
    upsert_new->SetFID(2);
    upsert_new->SetField("name", "upsert_new");
    OGRPoint pt3(50.0, 60.0);
    upsert_new->SetGeometry(&pt3);
    EXPECT_EQ(OGRERR_NONE, layer->UpsertFeature(upsert_new));
    OGRFeature::DestroyFeature(upsert_new);
  }

  EXPECT_EQ(2, layer->GetFeatureCount());
}

// Tests string truncation, non-UTF-8 laundering, and datetime timezone
// adjustments.
TEST_F(OGRGeoPackageTableLayerTest, TruncateFieldsAndDateTimeWithTz) {
  const char* const opts[] = {"TRUNCATE_FIELDS=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("tbl_trunc", nullptr, wkbNone,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_str("str_col", OFTString);
  f_str.SetWidth(4);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));

  OGRFieldDefn f_dt("dt_col", OFTDateTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  // 1. Truncate long string
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("str_col", "abcdefghij");
    feat->SetField("dt_col", "2023-05-10T14:30:00+02:00");
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 2. Launder invalid UTF-8 string
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("str_col", "ab\xff\xfe");
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
}

// Tests coordinate precision and reading with
// UNDO_DISCARD_COORD_LSB_ON_READING.
TEST_F(OGRGeoPackageTableLayerTest, CoordinatePrecisionAndUndoDiscardLsb) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const opts[] = {
      "COORDINATE_PRECISION=0.001",
      "DISCARD_COORD_LSB=YES",
      "UNDO_DISCARD_COORD_LSB_ON_READING=YES",
      nullptr,
  };
  OGRLayer* layer = dataset_->CreateLayer("tbl_prec", &srs, wkbPoint,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
  OGRPoint pt(12.34567, 45.67891);
  feat->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Close and reopen to exercise parsing CoordinatePrecision metadata on open
  dataset_.reset();
  dataset_.reset(GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, dataset_);
  layer = dataset_->GetLayerByName("tbl_prec");
  ASSERT_NE(nullptr, layer);

  // Read via Arrow stream
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  ASSERT_TRUE(layer->GetArrowStream(&stream, nullptr));

  struct ArrowArray array;
  memset(&array, 0, sizeof(array));
  EXPECT_EQ(0, stream.get_next(&stream, &array));
  if (array.release) array.release(&array);
  if (stream.release) stream.release(&stream);
}

// Tests Arrow streaming across all field types, including null values.
TEST_F(OGRGeoPackageTableLayerTest, ArrowStreamAllTypesAndNulls) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      dataset_->CreateLayer("tbl_arrow_types", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));

  OGRFieldDefn f_int16("f_int16", OFTInteger);
  f_int16.SetSubType(OFSTInt16);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int16));

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64));

  OGRFieldDefn f_float("f_float", OFTReal);
  f_float.SetSubType(OFSTFloat32);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_float));

  OGRFieldDefn f_real("f_real", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real));

  OGRFieldDefn f_bin("f_bin", OFTBinary);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bin));

  OGRFieldDefn f_date("f_date", OFTDate);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_date));

  OGRFieldDefn f_time("f_time", OFTTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_time));

  OGRFieldDefn f_dt("f_dt", OFTDateTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  OGRFieldDefn f_str("f_str", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));

  // Feature 1: All non-null
  {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("f_bool", 1);
    feat->SetField("f_int16", 123);
    feat->SetField("f_int64", 1234567890123LL);
    feat->SetField("f_float", 3.14f);
    feat->SetField("f_real", 2.71828);
    GByte bytes[] = {0x01, 0x02, 0x03};
    feat->SetField(feat->GetFieldIndex("f_bin"), 3, bytes);
    feat->SetField("f_date", "2023-01-15");
    feat->SetField("f_time", "12:34:56");
    feat->SetField("f_dt", "2023-01-15T12:34:56Z");
    feat->SetField("f_str", "hello");
    OGRPoint pt(1.0, 2.0);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // Feature 2: All null
  {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetGeometry(nullptr);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  ASSERT_TRUE(layer->GetArrowStream(&stream, nullptr));

  struct ArrowArray array;
  memset(&array, 0, sizeof(array));
  EXPECT_EQ(0, stream.get_next(&stream, &array));
  EXPECT_EQ(2, array.length);
  if (array.release) array.release(&array);
  if (stream.release) stream.release(&stream);
}

// Tests AlterFieldDefn on a column with a SQLite index and inside a
// transaction.
TEST_F(OGRGeoPackageTableLayerTest, AlterFieldDefnWithIndexAndTransactions) {
  OGRLayer* layer =
      dataset_->CreateLayer("tbl_idx_trans", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_name("col_to_index", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_name));
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Create an index in SQLite on col_to_index
  dataset_->ExecuteSQL("CREATE INDEX idx_col ON tbl_idx_trans (col_to_index)",
                       nullptr, nullptr);

  // Alter inside a transaction, then rollback
  EXPECT_EQ(OGRERR_NONE, layer->StartTransaction());
  OGRFieldDefn new_name("col_indexed_renamed", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->AlterFieldDefn(0, &new_name, ALTER_NAME_FLAG));
  EXPECT_STREQ("col_indexed_renamed",
               layer->GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(OGRERR_NONE, layer->RollbackTransaction());
}

// Tests UpdateFeature, async RTree cancellation, and spatial filter over RTree
// extent.
TEST_F(OGRGeoPackageTableLayerTest, UpdateFeatureAndAsyncRTreeInterruption) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("tbl_update_async", &srs, wkbPoint,
                                          const_cast<char**>(opts));
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_val("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_val));
  OGRFieldDefn f_uniq("code", OFTInteger);
  f_uniq.SetUnique(true);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_uniq));

  // Insert feature
  OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
  feat->SetFID(1);
  feat->SetField("val", 100);
  feat->SetField("code", 999);
  OGRPoint pt(5.0, 5.0);
  feat->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // 1. Test UpdateFeature directly
  {
    OGRFeature* upd = layer->GetFeature(1);
    ASSERT_NE(nullptr, upd);
    upd->SetField("val", 200);
    int updated_field = layer->GetLayerDefn()->GetFieldIndex("val");
    EXPECT_EQ(OGRERR_NONE,
              layer->UpdateFeature(upd, 1, &updated_field, 0, nullptr, false));
    OGRFeature::DestroyFeature(upd);
  }

  // 2. Upsert with NULL FID and unique column
  {
    OGRFeature* upsert_uniq = OGRFeature::CreateFeature(layer->GetLayerDefn());
    upsert_uniq->SetFID(OGRNullFID);
    upsert_uniq->SetField("val", 300);
    upsert_uniq->SetField("code", 999);
    OGRPoint pt2(6.0, 6.0);
    upsert_uniq->SetGeometry(&pt2);
    EXPECT_EQ(OGRERR_NONE, layer->UpsertFeature(upsert_uniq));
    OGRFeature::DestroyFeature(upsert_uniq);
  }

  // 3. Spatial filter covering the entire world / RTree bounding box
  {
    OGRPolygon world_poly;
    OGRLinearRing ring;
    ring.addPoint(-180, -90);
    ring.addPoint(180, -90);
    ring.addPoint(180, 90);
    ring.addPoint(-180, 90);
    ring.addPoint(-180, -90);
    world_poly.addRing(&ring);

    layer->SetSpatialFilter(&world_poly);
    layer->ResetReading();
    OGRFeature* f = layer->GetNextFeature();
    EXPECT_NE(nullptr, f);
    OGRFeature::DestroyFeature(f);
    layer->SetSpatialFilter(nullptr);
  }

  // 4. CreateField with Default and Unique after table creation
  {
    OGRFieldDefn f_dt_def("dt_def", OFTDateTime);
    f_dt_def.SetDefault("'2023/05/10 12:30:45'");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_dt_def));

    OGRFieldDefn f_dt_def2("dt_def2", OFTDateTime);
    f_dt_def2.SetDefault("'2023/05/10 12:30:45.123'");
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_dt_def2));

    OGRFieldDefn f_unique_add("u_add", OFTInteger);
    f_unique_add.SetUnique(true);
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    (void)layer->CreateField(&f_unique_add);
  }
}

// Tests CreateGeomField on existing layers, generated columns, and SQL
// defaults.
TEST_F(OGRGeoPackageTableLayerTest, CreateGeomFieldAndGeneratedColumns) {
  // 1. CreateGeomField on non-spatial table
  {
    OGRLayer* layer =
        dataset_->CreateLayer("tbl_add_geom", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

    // Bad geom type wkbNone
    OGRGeomFieldDefn bad_geom("bad", wkbNone);
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateGeomField(&bad_geom));

    // Valid geom field inside transaction, then rollback
    EXPECT_EQ(OGRERR_NONE, layer->StartTransaction());
    auto* srs = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, srs->importFromEPSG(4326));
    OGRGeomFieldDefn new_geom("", wkbPoint);
    new_geom.SetSpatialRef(srs);
    srs->Release();

    EXPECT_EQ(OGRERR_NONE, layer->CreateGeomField(&new_geom));
    EXPECT_EQ(1, layer->GetLayerDefn()->GetGeomFieldCount());

    // Cannot add second geom field
    OGRGeomFieldDefn second_geom("geom2", wkbPoint);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateGeomField(&second_geom));

    EXPECT_EQ(OGRERR_NONE, layer->RollbackTransaction());
  }

  // 2. Generated columns, datetime defaults, and string defaults in
  // ReadTableDefinition
  {
    dataset_->ExecuteSQL(
        "CREATE TABLE tbl_gen_cols ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "a INTEGER,"
        "b INTEGER GENERATED ALWAYS AS (a * 2) STORED,"
        "dt1 DATETIME DEFAULT '2023-05-10 12:30:45',"
        "dt2 DATETIME DEFAULT '2023-05-10 12:30:45.123',"
        "str_def TEXT DEFAULT hello)",
        nullptr, nullptr);
    dataset_->ExecuteSQL(
        "INSERT INTO gpkg_contents (table_name, data_type, identifier) "
        "VALUES ('tbl_gen_cols', 'attributes', 'tbl_gen_cols')",
        nullptr, nullptr);

    dataset_.reset();
    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, dataset_);

    OGRLayer* layer = dataset_->GetLayerByName("tbl_gen_cols");
    ASSERT_NE(nullptr, layer);
    OGRFeatureDefn* defn = layer->GetLayerDefn();
    ASSERT_NE(nullptr, defn);

    int idx_b = defn->GetFieldIndex("b");
    if (idx_b >= 0) {
      EXPECT_TRUE(defn->GetFieldDefn(idx_b)->IsGenerated());
    }
  }
}

// Tests DateTime UTC format and invalid date years.
TEST_F(OGRGeoPackageTableLayerTest, DateTimeUtcAndInvalidDateYears) {
  constexpr char kUtcGpkg[] = "/vsimem/test_dt_utc.gpkg";
  auto cleanup_vsi = absl::MakeCleanup([kUtcGpkg]() { VSIUnlink(kUtcGpkg); });

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const ds_opts[] = {"DATETIME_FORMAT=UTC", nullptr};
  std::unique_ptr<GDALDataset> ds(driver->Create(kUtcGpkg, 0, 0, 0, GDT_Unknown,
                                                 const_cast<char**>(ds_opts)));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("tbl_dt_utc", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_dt("f_dt", OFTDateTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));
  OGRFieldDefn f_d("f_d", OFTDate);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_d));

  // Feature with TZFlag = 0 and TZFlag = 108 (+02:00)
  {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField(0, 2023, 5, 10, 12, 0, 0.0f, 0);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);

    feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField(0, 2023, 5, 10, 14, 30, 0.0f, 108);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // Feature with invalid year (<0 or >=10000)
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField(1, 10005, 1, 1, 0, 0, 0.0f, 0);
    (void)layer->CreateFeature(feat);
    OGRFeature::DestroyFeature(feat);
  }
}

// Tests SRID handling in creation parameters and coordinate precision for ZM
// geometries.
TEST_F(OGRGeoPackageTableLayerTest,
       SetCreationParametersSridAndCoordPrecision) {
  // 1. SRID option variations
  {
    const char* const opts1[] = {"SRID=4326", nullptr};
    OGRLayer* l1 = dataset_->CreateLayer("tbl_srid1", nullptr, wkbPoint,
                                         const_cast<char**>(opts1));
    EXPECT_NE(nullptr, l1);

    const char* const opts2[] = {"SRID=99999", nullptr};
    OGRLayer* l2 = dataset_->CreateLayer("tbl_srid2", nullptr, wkbPoint,
                                         const_cast<char**>(opts2));
    EXPECT_NE(nullptr, l2);

    const char* const opts3[] = {"SRID=123456", nullptr};
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRLayer* l3 = dataset_->CreateLayer("tbl_srid3", nullptr, wkbPoint,
                                         const_cast<char**>(opts3));
    EXPECT_NE(nullptr, l3);
  }

  // 2. Coordinate precision via OGRGeomFieldDefn
  {
    OGRGeomFieldDefn geom_defn("geom_zm", wkbPointZM);
    OGRGeomCoordinatePrecision prec;
    prec.dfXYResolution = 0.001;
    prec.dfZResolution = 0.01;
    prec.dfMResolution = 0.1;
    geom_defn.SetCoordinatePrecision(prec);

    const char* const opts[] = {
        "DISCARD_COORD_LSB=YES",
        "UNDO_DISCARD_COORD_LSB_ON_READING=YES",
        nullptr,
    };
    OGRLayer* l = dataset_->CreateLayer("tbl_coord_prec_zm", &geom_defn,
                                        const_cast<char**>(opts));
    ASSERT_NE(nullptr, l);

    OGRFeature* feat = OGRFeature::CreateFeature(l->GetLayerDefn());
    OGRPoint pt(1.23456, 2.34567, 3.45678);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, l->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
    EXPECT_EQ(OGRERR_NONE, l->SyncToDisk());

    // Reopen dataset to test parsing precision metadata and Arrow reading
    dataset_.reset();
    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, dataset_);

    l = dataset_->GetLayerByName("tbl_coord_prec_zm");
    ASSERT_NE(nullptr, l);
    EXPECT_NEAR(0.001,
                l->GetLayerDefn()
                    ->GetGeomFieldDefn(0)
                    ->GetCoordinatePrecision()
                    .dfXYResolution,
                1e-6);

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT_TRUE(l->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }
}

#if !defined(THREAD_SANITIZER)
// Tests threaded RTree in-memory flush, thread cancellation, and thread worker
// errors.
// Upstream GDAL's OGRGeoPackageTableLayer::AsyncRTreeThreadFunction() contains
// an unsynchronized access to internal worker thread flags that triggers TSan.
TEST_F(OGRGeoPackageTableLayerTest, ThreadedRTreeFlushCancelAndError) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // Sub-test 1: In-memory RTree flush to disk + copy back
  {
    constexpr char kPath1[] = "/vsimem/rtree_thread1.gpkg";
    auto cleanup = absl::MakeCleanup([kPath1]() { VSIUnlink(kPath1); });

    CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", "YES");
    CPLSetConfigOption("OGR_GPKG_MAX_RAM_USAGE_RTREE", "1");
    auto cleanup_cfg = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", nullptr);
      CPLSetConfigOption("OGR_GPKG_MAX_RAM_USAGE_RTREE", nullptr);
    });

    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath1, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
    OGRLayer* layer = ds->CreateLayer("tbl-rtree-1", &srs, wkbPoint,
                                      const_cast<char**>(opts));
    ASSERT_NE(nullptr, layer);

    for (int i = 1; i <= 25; ++i) {
      OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
      OGRPoint pt(i, i);
      feat->SetGeometry(&pt);
      ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
      OGRFeature::DestroyFeature(feat);
    }
    EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());
  }

  // Sub-test 2: CancelAsyncRTree
  {
    constexpr char kPath2[] = "/vsimem/rtree_thread2.gpkg";
    auto cleanup = absl::MakeCleanup([kPath2]() { VSIUnlink(kPath2); });

    CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", "YES");
    auto cleanup_cfg = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", nullptr);
    });

    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath2, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
    OGRLayer* layer =
        ds->CreateLayer("tbl_t2", &srs, wkbPoint, const_cast<char**>(opts));
    ASSERT_NE(nullptr, layer);

    for (int i = 1; i <= 15; ++i) {
      OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
      feat->SetFID(i);
      OGRPoint pt(i, i);
      feat->SetGeometry(&pt);
      ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
      OGRFeature::DestroyFeature(feat);
    }
    // Delete feature to trigger CancelAsyncRTree() while background thread is
    // running
    EXPECT_EQ(OGRERR_NONE, layer->DeleteFeature(1));
    EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());
  }

  // Sub-test 3: Simulated error in RTree worker
  {
    constexpr char kPath3[] = "/vsimem/rtree_thread3.gpkg";
    auto cleanup = absl::MakeCleanup([kPath3]() { VSIUnlink(kPath3); });

    CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", "YES");
    CPLSetConfigOption("OGR_GPKG_MAX_RAM_USAGE_RTREE", "1");
    CPLSetConfigOption(
        "OGR_GPKG_SIMULATE_INSERT_INTO_MY_RTREE_PREPARATION_ERROR", "YES");
    auto cleanup_cfg = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", nullptr);
      CPLSetConfigOption("OGR_GPKG_MAX_RAM_USAGE_RTREE", nullptr);
      CPLSetConfigOption(
          "OGR_GPKG_SIMULATE_INSERT_INTO_MY_RTREE_PREPARATION_ERROR", nullptr);
    });

    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath3, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    const char* const opts[] = {"SPATIAL_INDEX=YES", nullptr};
    OGRLayer* layer =
        ds->CreateLayer("tbl_t3", &srs, wkbPoint, const_cast<char**>(opts));
    ASSERT_NE(nullptr, layer);

    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    for (int i = 1; i <= 25; ++i) {
      OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
      OGRPoint pt(i, i);
      feat->SetGeometry(&pt);
      (void)layer->CreateFeature(feat);
      OGRFeature::DestroyFeature(feat);
    }
    (void)layer->SyncToDisk();
  }
}
#endif

// Tests explicit CreateSpatialIndex, DropSpatialIndex, and GPKG 1.2 update1
// trigger workaround.
TEST_F(OGRGeoPackageTableLayerTest,
       SpatialIndexExplicitCreateDropAndVersion12Update1Trigger) {
  constexpr char kPathV12[] = "/vsimem/gpkg_v12.gpkg";
  auto cleanup = absl::MakeCleanup([kPathV12]() { VSIUnlink(kPathV12); });

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const ds_opts[] = {
      "VERSION=1.2",
      "ADD_GPKG_OGR_CONTENTS=YES",
      nullptr,
  };
  std::unique_ptr<GDALDataset> ds(driver->Create(kPathV12, 0, 0, 0, GDT_Unknown,
                                                 const_cast<char**>(ds_opts)));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const layer_opts[] = {"SPATIAL_INDEX=NO", nullptr};
  OGRLayer* layer = ds->CreateLayer("tbl_v12", &srs, wkbPoint,
                                    const_cast<char**>(layer_opts));
  ASSERT_NE(nullptr, layer);

  for (int i = 1; i <= 5; ++i) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetFID(i);
    OGRPoint pt(i, i);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
  ASSERT_NE(nullptr, gpkg_layer);

  // 1. DropSpatialIndex when none exists fails
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_FALSE(gpkg_layer->DropSpatialIndex());
  }

  // 2. CreateSpatialIndex succeeds
  EXPECT_TRUE(gpkg_layer->CreateSpatialIndex());

  // 3. CreateSpatialIndex when one already exists fails
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_FALSE(gpkg_layer->CreateSpatialIndex());
  }

  // 4. UpsertFeature triggers WorkaroundUpdate1TriggerIssue on GPKG 1.2
  {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetFID(1);
    OGRPoint pt(10.0, 10.0);
    feat->SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, layer->UpsertFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 5. RevertWorkaroundUpdate1TriggerIssue triggered on sync
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // 6. Deferred DropSpatialIndex followed by SyncToDisk
  EXPECT_TRUE(gpkg_layer->DropSpatialIndex(/* bDropRTreeTable = */ true));
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());
  EXPECT_FALSE(gpkg_layer->HasSpatialIndex());
}

// Tests handling of FID as a regular column (OFTReal, OFTInteger64) and
// consistency checks.
TEST_F(OGRGeoPackageTableLayerTest, FIDAsRegularColumnRealAndInt) {
  // 1. Layer with OFTReal FID column
  {
    const char* const opts[] = {"FID=my_fid", nullptr};
    OGRLayer* layer = dataset_->CreateLayer("tbl_fid_real", nullptr, wkbNone,
                                            const_cast<char**>(opts));
    ASSERT_NE(nullptr, layer);

    // Disallowed string type for FID field
    OGRFieldDefn f_str("my_fid", OFTString);
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateField(&f_str));

    // Allowed OFTReal with width 20 and precision 0
    OGRFieldDefn f_real("my_fid", OFTReal);
    f_real.SetWidth(20);
    f_real.SetPrecision(0);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_real));

    // Null FID with integer double value in field -> sets FID
    OGRFeature* feat1 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat1->SetFID(OGRNullFID);
    feat1->SetField("my_fid", 10.0);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat1));
    EXPECT_EQ(10, feat1->GetFID());
    OGRFeature::DestroyFeature(feat1);

    // Null FID with non-integer double value in field -> fails
    OGRFeature* feat2 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat2->SetFID(OGRNullFID);
    feat2->SetField("my_fid", 10.5);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(feat2));
    OGRFeature::DestroyFeature(feat2);

    // Inconsistent FID and field value -> fails
    OGRFeature* feat3 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat3->SetFID(20);
    feat3->SetField("my_fid", 99.0);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(feat3));
    OGRFeature::DestroyFeature(feat3);
  }

  // 2. Layer with OFTInteger64 FID column
  {
    const char* const opts[] = {"FID=id_col", nullptr};
    OGRLayer* layer = dataset_->CreateLayer("tbl_fid_int", nullptr, wkbNone,
                                            const_cast<char**>(opts));
    ASSERT_NE(nullptr, layer);

    OGRFieldDefn f_int("id_col", OFTInteger64);
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_int));

    // Consistent FID
    OGRFeature* feat1 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat1->SetFID(1);
    feat1->SetField("id_col", 1);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat1));
    OGRFeature::DestroyFeature(feat1);

    // Inconsistent FID on CreateFeature
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* feat2 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat2->SetFID(2);
    feat2->SetField("id_col", 99);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(feat2));
    OGRFeature::DestroyFeature(feat2);

    // Inconsistent FID on SetFeature
    OGRFeature* feat3 = layer->GetFeature(1);
    ASSERT_NE(nullptr, feat3);
    feat3->SetField("id_col", 100);
    EXPECT_EQ(OGRERR_FAILURE, layer->SetFeature(feat3));

    // Inconsistent FID on UpdateFeature
    int updated_field = layer->GetLayerDefn()->GetFieldIndex("id_col");
    EXPECT_EQ(OGRERR_FAILURE, layer->UpdateFeature(feat3, 1, &updated_field, 0,
                                                   nullptr, false));
    OGRFeature::DestroyFeature(feat3);
  }
}

// Tests AlterFieldDefn outside transactions, index recreation, domains, and
// auxiliary tables.
TEST_F(OGRGeoPackageTableLayerTest,
       AlterFieldOutsideTransactionAndDomainsAndAuxiliaryTables) {
  // 1. Add coded field domain to dataset
  std::vector<OGRCodedValue> coded_values;
  coded_values.push_back({CPLStrdup("1"), CPLStrdup("Active")});
  coded_values.push_back({CPLStrdup("2"), CPLStrdup("Inactive")});
  auto domain = std::make_unique<OGRCodedFieldDomain>(
      "status_domain", "Status description", OFTInteger, OFSTNone,
      std::move(coded_values));
  std::string failure_reason;
  EXPECT_TRUE(dataset_->AddFieldDomain(std::move(domain), failure_reason));

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      dataset_->CreateLayer("tbl_aux_alter", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  layer->SetMetadataItem("MD_KEY", "MD_VAL");
  dataset_->FlushCache(false);

  OGRFieldDefn f_dom("f_dom", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dom));
  OGRFieldDefn f_idx("f_idx", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_idx));
  OGRFieldDefn f_del("f_del", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_del));
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Create SQLite index and trigger on tbl_aux_alter
  dataset_->ExecuteSQL("CREATE INDEX idx_f_idx ON tbl_aux_alter (f_idx)",
                       nullptr, nullptr);
  dataset_->ExecuteSQL(
      "CREATE TRIGGER trig_test AFTER INSERT ON tbl_aux_alter BEGIN SELECT 1; "
      "END",
      nullptr, nullptr);

  // Insert metadata and relationships referencing f_idx and f_del
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_data_columns (table_name, column_name, name) "
      "VALUES ('tbl_aux_alter', 'f_del', 'del_name')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_metadata_reference (reference_scope, table_name, "
      "column_name, md_file_id) "
      "VALUES ('column', 'tbl_aux_alter', 'f_del', 101)",
      nullptr, nullptr);

  dataset_->ExecuteSQL(
      "CREATE TABLE IF NOT EXISTS gpkgext_relations ("
      "id INTEGER PRIMARY KEY,"
      "base_table_name TEXT,"
      "base_primary_column TEXT,"
      "related_table_name TEXT,"
      "related_primary_column TEXT,"
      "relation_name TEXT,"
      "mapping_table_name TEXT)",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO gpkgext_relations (base_table_name, base_primary_column) "
      "VALUES ('tbl_aux_alter', 'f_idx')",
      nullptr, nullptr);

  dataset_->ExecuteSQL("PRAGMA foreign_keys = ON", nullptr, nullptr);
  CPLSetConfigOption("OGR_GPKG_INTEGRITY_CHECK", "YES");
  CPLSetConfigOption("OGR_GPKG_FOREIGN_KEY_CHECK", "YES");
  auto cleanup_checks = absl::MakeCleanup([]() {
    CPLSetConfigOption("OGR_GPKG_INTEGRITY_CHECK", nullptr);
    CPLSetConfigOption("OGR_GPKG_FOREIGN_KEY_CHECK", nullptr);
  });

  // 2. AlterFieldDefn outside transaction -> raw schema rewrite, index
  // recreate, auxiliary rename
  {
    OGRFieldDefn new_idx("f_idx_renamed", OFTString);
    new_idx.SetWidth(50);
    EXPECT_EQ(OGRERR_NONE,
              layer->AlterFieldDefn(
                  1, &new_idx, ALTER_NAME_FLAG | ALTER_WIDTH_PRECISION_FLAG));
    EXPECT_STREQ("f_idx_renamed",
                 layer->GetLayerDefn()->GetFieldDefn(1)->GetNameRef());
  }

  // 3. AlterFieldDefn with ALTER_DOMAIN_FLAG
  {
    OGRFieldDefn new_dom("f_dom", OFTInteger);
    new_dom.SetDomainName("status_domain");
    EXPECT_EQ(OGRERR_NONE,
              layer->AlterFieldDefn(0, &new_dom, ALTER_DOMAIN_FLAG));
    EXPECT_STREQ(
        "status_domain",
        layer->GetLayerDefn()->GetFieldDefn(0)->GetDomainName().c_str());
  }

  // 4. DeleteField on f_del -> deletes from gpkg_data_columns and
  // gpkg_metadata_reference
  {
    int del_idx = layer->GetLayerDefn()->GetFieldIndex("f_del");
    ASSERT_GE(del_idx, 0);
    EXPECT_EQ(OGRERR_NONE, layer->DeleteField(del_idx));
  }

  // 5. AlterGeomFieldDefn with coordinate epoch flag
  {
    auto* epoch_srs = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, epoch_srs->importFromEPSG(4326));
    epoch_srs->SetCoordinateEpoch(2022.5);
    OGRGeomFieldDefn new_geom("geom", wkbPoint);
    new_geom.SetSpatialRef(epoch_srs);

    EXPECT_EQ(OGRERR_NONE, layer->AlterGeomFieldDefn(
                               0, &new_geom,
                               ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG |
                                   ALTER_GEOM_FIELD_DEFN_SRS_FLAG));

    // Also call with ONLY ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG
    epoch_srs->SetCoordinateEpoch(2023.0);
    EXPECT_EQ(OGRERR_NONE,
              layer->AlterGeomFieldDefn(
                  0, &new_geom, ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG));
    epoch_srs->Release();
  }

  // 6. Rename layer updating auxiliary tables
  EXPECT_EQ(OGRERR_NONE, layer->Rename("tbl_aux_renamed"));
}

static int DummyProgressCancel(double, const char*, void*) {
  return 0;  // 0 signals cancel in GDAL progress functions
}

// Tests GetGeometryTypes, GetExtent3D, unknown extensions, and style table
// rename.
TEST_F(OGRGeoPackageTableLayerTest, GeometryTypes3DExtentAndUnknownExtensions) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      dataset_->CreateLayer("tbl_geom_types", &srs, wkbUnknown, nullptr);
  ASSERT_NE(nullptr, layer);

  // Insert Point25D, LineString25D, and a GeometryCollection containing TINZ
  {
    OGRFeature* f1 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRPoint p(1, 2, 3);
    f1->SetGeometry(&p);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(f1));
    OGRFeature::DestroyFeature(f1);

    OGRFeature* f2 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(1, 1, 10);
    ls.addPoint(2, 2, 20);
    f2->SetGeometry(&ls);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(f2));
    OGRFeature::DestroyFeature(f2);

    OGRFeature* f3 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    OGRGeometryCollection gc;
    gc.set3D(true);
    auto* tin = new OGRTriangulatedSurface();
    tin->set3D(true);
    gc.addGeometryDirectly(tin);
    f3->SetGeometry(&gc);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(f3));
    OGRFeature::DestroyFeature(f3);
  }
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // 1. GetGeometryTypes
  int nEntries = 0;
  OGRGeometryTypeCounter* counters = layer->GetGeometryTypes(
      0, OGR_GGT_GEOMCOLLECTIONZ_TINZ, nEntries, nullptr, nullptr);
  EXPECT_NE(nullptr, counters);
  EXPECT_GT(nEntries, 0);
  CPLFree(counters);

  // Cancelled progress
  counters = layer->GetGeometryTypes(
      0, OGR_GGT_GEOMCOLLECTIONZ_TINZ | OGR_GGT_STOP_IF_MIXED, nEntries,
      DummyProgressCancel, nullptr);
  CPLFree(counters);

  // Invalid geom field
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    EXPECT_EQ(nullptr,
              layer->GetGeometryTypes(-1, 0, nEntries, nullptr, nullptr));
  }

  // 2. GetExtent3D on 3D layer and 2D layer
  OGREnvelope3D env3d;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent3D(0, &env3d, true));
  EXPECT_NEAR(1.0, env3d.MinX, 1e-4);
  EXPECT_NEAR(3.0, env3d.MinZ, 1e-4);
  EXPECT_NEAR(20.0, env3d.MaxZ, 1e-4);

  OGRLayer* l2d = dataset_->CreateLayer("tbl_2d", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, l2d);
  EXPECT_EQ(OGRERR_FAILURE, l2d->GetExtent3D(0, &env3d, true));

  // 3. Unknown extensions in gpkg_extensions
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('tbl_geom_types', 'geom', 'unknown_rw', 'http://def', "
      "'read-write')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('tbl_geom_types', 'geom', 'unknown_wo', 'http://def', "
      "'write-only')",
      nullptr, nullptr);

  // Reopen in update mode (emits warning/error for unknown extensions)
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    dataset_.reset();
    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, dataset_);
    EXPECT_NE(nullptr, dataset_->GetLayerByName("tbl_geom_types"));
  }

  // Reopen in read-only mode
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    dataset_.reset();
    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, dataset_);
    EXPECT_NE(nullptr, dataset_->GetLayerByName("tbl_geom_types"));
  }

  // 4. Rename with layer_styles table present
  {
    dataset_.reset();
    dataset_.reset(
        GDALDataset::Open(kTestGpkg, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, dataset_);
    OGRLayer* l_rename = dataset_->GetLayerByName("tbl_geom_types");
    ASSERT_NE(nullptr, l_rename);

    dataset_->ExecuteSQL(
        "CREATE TABLE IF NOT EXISTS layer_styles ("
        "id INTEGER PRIMARY KEY,"
        "f_table_name TEXT,"
        "f_geometry_column TEXT,"
        "styleName TEXT)",
        nullptr, nullptr);
    dataset_->ExecuteSQL(
        "INSERT INTO layer_styles (f_table_name, f_geometry_column, "
        "styleName) "
        "VALUES ('tbl_geom_types', 'geom', 'default_style')",
        nullptr, nullptr);

    EXPECT_EQ(OGRERR_NONE, l_rename->Rename("tbl_geom_types_renamed"));
    EXPECT_NE(nullptr, dataset_->GetLayerByName("tbl_geom_types_renamed"));
  }
}

// Tests Arrow streaming edge cases: spatial filter, gaps in FID numbering,
// base implementation fallback, and thread configurations.
TEST_F(OGRGeoPackageTableLayerTest, ArrowStreamSpatialFilterBaseImplAndGaps) {
  constexpr char kPath[] = "/vsimem/arrow_edge.gpkg";
  auto cleanup = absl::MakeCleanup([kPath]() { VSIUnlink(kPath); });

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const ds_opts[] = {
      "ADD_GPKG_OGR_CONTENTS=YES",
      nullptr,
  };
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(ds_opts)));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const char* const l_opts[] = {"SPATIAL_INDEX=YES", nullptr};
  OGRLayer* layer1 =
      ds->CreateLayer("tbl_gap", &srs, wkbPoint, const_cast<char**>(l_opts));
  ASSERT_NE(nullptr, layer1);

  // Insert features with FID gap: 1, 2, 5 (min=1, max=5, total=3 -> max !=
  // total)
  for (int fid : {1, 2, 5}) {
    OGRFeature* f = OGRFeature::CreateFeature(layer1->GetLayerDefn());
    f->SetFID(fid);
    OGRPoint pt(fid, fid);
    f->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer1->CreateFeature(f));
    OGRFeature::DestroyFeature(f);
  }
  EXPECT_EQ(OGRERR_NONE, layer1->SyncToDisk());

  // Second layer where min(FID) != 1
  OGRLayer* layer2 =
      ds->CreateLayer("tbl_nomin", &srs, wkbPoint, const_cast<char**>(l_opts));
  ASSERT_NE(nullptr, layer2);
  {
    OGRFeature* f = OGRFeature::CreateFeature(layer2->GetLayerDefn());
    f->SetFID(10);
    OGRPoint pt(10, 10);
    f->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer2->CreateFeature(f));
    OGRFeature::DestroyFeature(f);
  }
  EXPECT_EQ(OGRERR_NONE, layer2->SyncToDisk());

  // 1. Read layer1 where max(FID) != total count (hits line 8667)
  {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer1->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(3, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // 2. Read layer2 where min(FID) != 1 (hits line 8676)
  {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer2->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(1, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // 3. Spatial filter smaller than layer extent (hits
  // GetNextArrowArrayAsynchronousWorker RTree join at lines 8399, 8535-8568,
  // and envelope filter at 8041-8046)
  {
    layer1->ResetReading();
    layer1->SetSpatialFilterRect(0.5, 0.5, 2.5, 2.5);
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer1->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(2, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // 4. Spatial filter covering whole layer extent (hits bUseSpatialIndex =
  // false at lines 8542-8552)
  {
    layer1->ResetReading();
    layer1->SetSpatialFilterRect(-100.0, -100.0, 100.0, 100.0);
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer1->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(3, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
    layer1->SetSpatialFilter(nullptr);
  }

  // 5. Arrow stream when m_iNextShapeId > 0 (hits lines 8524-8528, 8576-8579)
  {
    layer1->ResetReading();
    OGRFeature* f = layer1->GetNextFeature();
    ASSERT_NE(nullptr, f);
    OGRFeature::DestroyFeature(f);

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer1->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(2, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // 6. Base implementation fallback (hits line 8641)
  {
    CPLSetConfigOption("OGR_GPKG_STREAM_BASE_IMPL", "YES");
    auto cleanup_opt = absl::MakeCleanup(
        []() { CPLSetConfigOption("OGR_GPKG_STREAM_BASE_IMPL", nullptr); });

    layer1->ResetReading();
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer1->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(3, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // 7. Multi-threaded Arrow reading in read-only mode (hits GetThreadsAvailable
  // at line 8797)
  {
    ds.reset();
    CPLSetConfigOption("OGR_GPKG_NUM_THREADS", "ALL_CPUS");
    auto cleanup_th = absl::MakeCleanup(
        []() { CPLSetConfigOption("OGR_GPKG_NUM_THREADS", nullptr); });

    std::unique_ptr<GDALDataset> ro_ds(
        GDALDataset::Open(kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ro_ds);
    OGRLayer* ro_layer = ro_ds->GetLayerByName("tbl_gap");
    ASSERT_NE(nullptr, ro_layer);

    const char* const bopts[] = {"MAX_FEATURES_IN_BATCH=1", nullptr};
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(ro_layer->GetArrowStream(&stream, const_cast<char**>(bopts)));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    while (stream.get_next(&stream, &array) == 0 && array.release != nullptr) {
      array.release(&array);
    }
    if (stream.release) stream.release(&stream);
  }
}

// Tests direct methods, transaction commit jobs, deferred RTree drops,
// and geometry update workflows.
TEST_F(OGRGeoPackageTableLayerTest,
       DirectMethodsAndTransactionDeferredRTreeAndUpdateGeometry) {
  constexpr char kPath[] = "/vsimem/direct_methods.gpkg";
  auto cleanup = absl::MakeCleanup([kPath]() { VSIUnlink(kPath); });

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const ds_opts[] = {
      "ADD_GPKG_OGR_CONTENTS=YES",
      nullptr,
  };
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(ds_opts)));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. Layer created with IDENTIFIER and DESCRIPTION LCO (hits lines 6289-6293)
  const char* const l_opts[] = {
      "IDENTIFIER=layer_id",
      "DESCRIPTION=layer_description",
      "SPATIAL_INDEX=YES",
      nullptr,
  };
  OGRLayer* layer =
      ds->CreateLayer("tbl_direct", &srs, wkbPoint, const_cast<char**>(l_opts));
  ASSERT_NE(nullptr, layer);

  EXPECT_STREQ("layer_id", layer->GetMetadataItem("IDENTIFIER"));
  EXPECT_STREQ("layer_description", layer->GetMetadataItem("DESCRIPTION"));
  char** md = layer->GetMetadata();
  EXPECT_NE(nullptr, md);

  auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
  ASSERT_NE(nullptr, gpkg_layer);

  OGRFeature* feat0 = OGRFeature::CreateFeature(layer->GetLayerDefn());
  OGRPoint pt0(1.0, 1.0);
  feat0->SetGeometry(&pt0);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat0));
  OGRFeature::DestroyFeature(feat0);
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // 2. HasFastSpatialFilter & RecomputeExtent (hits lines 4373-4379, 5535-5541)
  EXPECT_TRUE(gpkg_layer->HasFastSpatialFilter(0));
  EXPECT_FALSE(gpkg_layer->HasFastSpatialFilter(-1));
  EXPECT_FALSE(gpkg_layer->HasFastSpatialFilter(100));

  gpkg_layer->RecomputeExtent();

  // 3. DropSpatialIndex(true) + RunDeferredDropRTreeTableIfNecessary (hits
  // lines 5235-5273)
  EXPECT_TRUE(gpkg_layer->DropSpatialIndex(true));
  EXPECT_TRUE(gpkg_layer->RunDeferredDropRTreeTableIfNecessary());
  EXPECT_FALSE(gpkg_layer->HasSpatialIndex());

  // Re-create spatial index
  EXPECT_TRUE(gpkg_layer->CreateSpatialIndex());

  // 4. DoJobAtTransactionCommit without threaded RTree (hits lines 3804-3809)
  {
    CPLSetConfigOption("OGR_GPKG_ALLOW_THREADED_RTREE", "NO");
    auto cleanup_allow = absl::MakeCleanup(
        []() { CPLSetConfigOption("OGR_GPKG_ALLOW_THREADED_RTREE", nullptr); });

    const char* const tx_opts[] = {"SPATIAL_INDEX=YES", nullptr};
    OGRLayer* tx_layer =
        ds->CreateLayer("tbl_tx", &srs, wkbPoint, const_cast<char**>(tx_opts));
    ASSERT_NE(nullptr, tx_layer);

    EXPECT_EQ(OGRERR_NONE, ds->StartTransaction());
    // Add field inside transaction then delete it (hits lines 1862-1866,
    // 6615-6628)
    OGRFieldDefn f_tx("f_temp", OFTString);
    EXPECT_EQ(OGRERR_NONE, tx_layer->CreateField(&f_tx));
    int f_idx = tx_layer->GetLayerDefn()->GetFieldIndex("f_temp");
    EXPECT_GE(f_idx, 0);
    EXPECT_EQ(OGRERR_NONE, tx_layer->DeleteField(f_idx));

    for (int i = 1; i <= 5; ++i) {
      OGRFeature* f = OGRFeature::CreateFeature(tx_layer->GetLayerDefn());
      OGRPoint pt(i, i);
      f->SetGeometry(&pt);
      ASSERT_EQ(OGRERR_NONE, tx_layer->CreateFeature(f));
      OGRFeature::DestroyFeature(f);
    }
    EXPECT_EQ(OGRERR_NONE, ds->CommitTransaction());
  }

  // 5. UpdateFeature updating geometry then SetFeature (hits lines 3153-3157,
  // 3270-3275, 3440-3449)
  {
    OGRFieldDefn f_val("f_val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_val));

    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("f_val", 100);
    OGRPoint pt(10.0, 10.0);
    feat->SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));

    // Update with geometry field updated
    OGRPoint pt_updated(20.0, 20.0);
    feat->SetGeometry(&pt_updated);
    int geom_field_idx = 0;
    EXPECT_EQ(OGRERR_NONE, layer->UpdateFeature(feat, 0, nullptr, 1,
                                                &geom_field_idx, false));

    // Calling SetFeature right after uses/resets m_poUpdateStatement (hits
    // lines 3153-3157)
    feat->SetField("f_val", 200);
    EXPECT_EQ(OGRERR_NONE, layer->SetFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // 6. Unique constraint violation in CreateFeature / SetFeature /
  // UpdateFeature (hits lines 2531-2539, 3191-3195, 3416-3420)
  {
    OGRLayer* uniq_layer = ds->CreateLayer("tbl_uniq", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, uniq_layer);
    OGRFieldDefn f_unique("uniq_code", OFTString);
    f_unique.SetNullable(false);
    ASSERT_EQ(OGRERR_NONE, uniq_layer->CreateField(&f_unique));
    EXPECT_EQ(OGRERR_NONE, uniq_layer->SyncToDisk());
    ds->ExecuteSQL("CREATE UNIQUE INDEX idx_uniq_code ON tbl_uniq(uniq_code)",
                   nullptr, nullptr);

    OGRFeature* f1 = OGRFeature::CreateFeature(uniq_layer->GetLayerDefn());
    f1->SetField("uniq_code", "CODE1");
    OGRPoint pt1(1, 1);
    f1->SetGeometry(&pt1);
    ASSERT_EQ(OGRERR_NONE, uniq_layer->CreateFeature(f1));

    OGRFeature* f2 = OGRFeature::CreateFeature(uniq_layer->GetLayerDefn());
    f2->SetField("uniq_code", "CODE2");
    OGRPoint pt2(2, 2);
    f2->SetGeometry(&pt2);
    ASSERT_EQ(OGRERR_NONE, uniq_layer->CreateFeature(f2));

    // Violate unique constraint on CreateFeature (hits lines 2531-2539)
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    OGRFeature* f_dup = OGRFeature::CreateFeature(uniq_layer->GetLayerDefn());
    f_dup->SetField("uniq_code", "CODE1");
    f_dup->SetGeometry(&pt1);
    EXPECT_EQ(OGRERR_FAILURE, uniq_layer->CreateFeature(f_dup));
    OGRFeature::DestroyFeature(f_dup);

    // Violate unique constraint on SetFeature (hits lines 3191-3195)
    f2->SetField("uniq_code", "CODE1");
    EXPECT_EQ(OGRERR_FAILURE, uniq_layer->SetFeature(f2));

    // Violate unique constraint on UpdateFeature (hits lines 3416-3420)
    int code_idx = uniq_layer->GetLayerDefn()->GetFieldIndex("uniq_code");
    EXPECT_EQ(OGRERR_FAILURE,
              uniq_layer->UpdateFeature(f2, 1, &code_idx, 0, nullptr, false));

    OGRFeature::DestroyFeature(f1);
    OGRFeature::DestroyFeature(f2);
  }

  // 7. Insert Point with M value into 2D layer (hits lines 2220-2225)
  {
    OGRLayer* m_layer = ds->CreateLayer("tbl_m", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, m_layer);

    OGRFeature* f_m = OGRFeature::CreateFeature(m_layer->GetLayerDefn());
    OGRPoint pt_m(5, 5);
    pt_m.setM(10.0);
    f_m->SetGeometry(&pt_m);
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    ASSERT_EQ(OGRERR_NONE, m_layer->CreateFeature(f_m));
    OGRFeature::DestroyFeature(f_m);
  }

  // 8. Polygon spatial filter on indexed layer (hits lines 3593-3599,
  // 4117-4137, 4170-4183)
  {
    const char* const poly_opts[] = {"SPATIAL_INDEX=YES", nullptr};
    OGRLayer* poly_layer = ds->CreateLayer("tbl_poly_filter", &srs, wkbPolygon,
                                           const_cast<char**>(poly_opts));
    ASSERT_NE(nullptr, poly_layer);

    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(10, 0);
    ring.addPoint(10, 10);
    ring.addPoint(0, 10);
    ring.addPoint(0, 0);
    OGRPolygon poly;
    poly.addRing(&ring);

    OGRFeature* feat = OGRFeature::CreateFeature(poly_layer->GetLayerDefn());
    feat->SetGeometry(&poly);
    ASSERT_EQ(OGRERR_NONE, poly_layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
    EXPECT_EQ(OGRERR_NONE, poly_layer->SyncToDisk());

    // Spatial filter polygon
    OGRLinearRing filter_ring;
    filter_ring.addPoint(2, 2);
    filter_ring.addPoint(8, 2);
    filter_ring.addPoint(8, 8);
    filter_ring.addPoint(2, 8);
    filter_ring.addPoint(2, 2);
    OGRPolygon filter_poly;
    filter_poly.addRing(&filter_ring);

    poly_layer->SetSpatialFilter(&filter_poly);
    EXPECT_EQ(1, poly_layer->GetFeatureCount(true));

    poly_layer->ResetReading();
    OGRFeature* f_read = poly_layer->GetNextFeature();
    ASSERT_NE(nullptr, f_read);
    OGRFeature::DestroyFeature(f_read);

    // Also test GetFeatureCount with NULL count in gpkg_ogr_contents (hits
    // lines 4148, 4170-4183)
    poly_layer->SetSpatialFilter(nullptr);
    ds->ExecuteSQL(
        "UPDATE gpkg_ogr_contents SET feature_count = NULL "
        "WHERE table_name = 'tbl_poly_filter'",
        nullptr, nullptr);
    EXPECT_EQ(1, poly_layer->GetFeatureCount(true));
  }
}

// Tests ReadTableDefinition edge cases: many layers, generated columns,
// datetime defaults, and unrecognized types.
TEST_F(OGRGeoPackageTableLayerTest, ReadTableDefinitionEdgeCasesAndManyLayers) {
  constexpr char kPath[] = "/vsimem/read_def_edge.gpkg";
  auto cleanup = absl::MakeCleanup([kPath]() { VSIUnlink(kPath); });

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // Create 12 layers to exceed THRESHOLD_GET_SQLITE_MASTER (10)
    for (int i = 0; i < 12; ++i) {
      std::string name = std::format("tbl_many_{}", i);
      OGRLayer* l = ds->CreateLayer(name.c_str(), &srs, wkbPoint, nullptr);
      ASSERT_NE(nullptr, l);
      OGRFieldDefn f(std::format("f_{}", i).c_str(), OFTString);
      if (i == 0) {
        f.SetAlternativeName("alt_0");
        f.SetUnique(true);
      }
      ASSERT_EQ(OGRERR_NONE, l->CreateField(&f));
      EXPECT_EQ(OGRERR_NONE, l->SyncToDisk());
    }

    // Create table with generated column and datetime defaults
    ds->ExecuteSQL(
        "CREATE TABLE tbl_custom ("
        "id INTEGER PRIMARY KEY,"
        "geom POLYGON NOT NULL,"
        "col_gen INTEGER GENERATED ALWAYS AS (id * 2),"
        "dt1 DATETIME DEFAULT '2023-05-01T12:34:56Z',"
        "dt2 DATETIME DEFAULT '2023-05-01T12:34:56.789Z',"
        "second_geom POINT,"
        "weird_geom WEIRD_TYPE)",
        nullptr, nullptr);

    ds->ExecuteSQL(
        "INSERT INTO gpkg_data_columns (table_name, column_name, name) "
        "VALUES ('tbl_custom', 'dt1', 'alt_dt1')",
        nullptr, nullptr);

    ds->ExecuteSQL(
        "INSERT INTO gpkg_contents (table_name, data_type, identifier) "
        "VALUES ('tbl_custom', 'features', 'tbl_custom')",
        nullptr, nullptr);

    ds->ExecuteSQL(
        "INSERT INTO gpkg_geometry_columns (table_name, column_name, "
        "geometry_type_name, srs_id, z, m) "
        "VALUES ('tbl_custom', 'geom', 'POINT', 4326, 0, 0)",
        nullptr, nullptr);

    // Also register an unknown geometry column type
    ds->ExecuteSQL(
        "INSERT INTO gpkg_geometry_columns (table_name, column_name, "
        "geometry_type_name, srs_id, z, m) "
        "VALUES ('tbl_custom', 'weird_geom', 'WEIRD_TYPE', 4326, 0, 0)",
        nullptr, nullptr);

    // Table missing from gpkg_geometry_columns
    ds->ExecuteSQL(
        "CREATE TABLE tbl_no_gpkg_geom (id INTEGER PRIMARY KEY, geom BLOB)",
        nullptr, nullptr);
    ds->ExecuteSQL(
        "INSERT INTO gpkg_contents (table_name, data_type, identifier) "
        "VALUES ('tbl_no_gpkg_geom', 'features', 'tbl_no_gpkg_geom')",
        nullptr, nullptr);
  }

  // Reopen to trigger ReadTableDefinition on all tables
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    const char* const open_opts[] = {"LIST_ALL_TABLES=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);

    OGRLayer* l_custom = ds->GetLayerByName("tbl_custom");
    ASSERT_NE(nullptr, l_custom);
    EXPECT_NE(nullptr, l_custom->GetLayerDefn());

    OGRLayer* l_no_geom = ds->GetLayerByName("tbl_no_gpkg_geom");
    EXPECT_EQ(nullptr, l_no_geom);

    // Check layer count
    EXPECT_GE(ds->GetLayerCount(), 13);
  }
}

// Tests remaining edge cases: default value expressions, Arrow memory limits,
// function argument limits, and SpatiaLite blobs.
TEST_F(OGRGeoPackageTableLayerTest,
       GeometryTypesArrowErrorRecoveryAndSchemaAlteration) {
  constexpr char kPath[] = "/vsimem/rem_edge.gpkg";
  auto cleanup = absl::MakeCleanup([kPath]() { VSIUnlink(kPath); });

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const ds_opts[] = {
      "ADD_GPKG_OGR_CONTENTS=YES",
      nullptr,
  };
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(ds_opts)));
  ASSERT_NE(nullptr, ds);
  auto* gpkg_ds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gpkg_ds);
  sqlite3* hDB = gpkg_ds->GetDB();
  ASSERT_NE(nullptr, hDB);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. Layer created with IDENTIFIER and SetMetadata(md, "") (hits lines
  // 6289-6293)
  const char* const l_opts[] = {
      "IDENTIFIER=layer_id2",
      "DESCRIPTION=layer_desc2",
      nullptr,
  };
  OGRLayer* layer =
      ds->CreateLayer("tbl_rem", &srs, wkbPoint, const_cast<char**>(l_opts));
  ASSERT_NE(nullptr, layer);

  // Call SetMetadata with domain "" (hits lines 6289-6293)
  char* empty_md[] = {nullptr};
  EXPECT_EQ(CE_None, layer->SetMetadata(empty_md, ""));

  // 2. Field creation with datetime defaults (hits lines 5957-5975)
  OGRFieldDefn f_dt1("f_dt1", OFTDateTime);
  f_dt1.SetDefault("2023-01-01T12:00:00Z");
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt1));

  OGRFieldDefn f_dt2("f_dt2", OFTDateTime);
  f_dt2.SetDefault("CURRENT_TIMESTAMP");
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt2));

  OGRFieldDefn f_dt3("f_dt3", OFTDateTime);
  f_dt3.SetDefault("(strftime('%Y-%m-%d %H:%M:%f','now'))");
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt3));

  // Insert a feature with defaults
  OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
  OGRPoint pt(1.0, 1.0);
  feat->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);
  EXPECT_EQ(OGRERR_NONE, layer->SyncToDisk());

  // Add field with NOT NULL and default on materialized table (hits lines 1803,
  // 1837, 1844-1845)
  OGRFieldDefn f_nn("f_nn", OFTString);
  f_nn.SetNullable(false);
  f_nn.SetDefault("''");
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_nn));

  // 3. Arrow array MAX_MEMORY limit (hits lines 7868-7878, 8749-8752,
  // 8066-8070)
  {
    // Sync mode with MAX_MEMORY
    const char* const mem_opts[] = {"MAX_MEMORY=1", nullptr};
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer->GetArrowStream(&stream, const_cast<char**>(mem_opts)));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    while (stream.get_next(&stream, &array) == 0 && array.release != nullptr) {
      array.release(&array);
    }
    if (stream.release) stream.release(&stream);

    // Async mode with filter and MAX_MEMORY
    layer->ResetReading();
    layer->SetAttributeFilter("FID >= 1");
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer->GetArrowStream(&stream, const_cast<char**>(mem_opts)));
    memset(&array, 0, sizeof(array));
    while (stream.get_next(&stream, &array) == 0 && array.release != nullptr) {
      array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    layer->SetAttributeFilter(nullptr);
  }

  // 4. sqlite3_limit SQLITE_LIMIT_FUNCTION_ARG = 3 (hits lines 8376-8380,
  // 8989-8994)
  {
    auto* gpkg_layer = dynamic_cast<OGRGeoPackageTableLayer*>(layer);
    ASSERT_NE(nullptr, gpkg_layer);

    int old_lim = sqlite3_limit(hDB, SQLITE_LIMIT_FUNCTION_ARG, 3);
    auto cleanup_lim = absl::MakeCleanup([hDB, old_lim]() {
      sqlite3_limit(hDB, SQLITE_LIMIT_FUNCTION_ARG, old_lim);
    });

    layer->ResetReading();
    layer->SetAttributeFilter("FID >= 1");
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(layer->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    while (stream.get_next(&stream, &array) == 0 && array.release != nullptr) {
      array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    layer->SetAttributeFilter(nullptr);
  }

  // 5. Insert SpatiaLite geometry blob using SQL and test GetExtent3D and Arrow
  // (hits lines 9103-9114 and 8000-8025)
  {
    OGRLayer* sl_layer =
        ds->CreateLayer("tbl_spatialite", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, sl_layer);
    EXPECT_EQ(OGRERR_NONE, sl_layer->SyncToDisk());

    GByte* sl_data = nullptr;
    int sl_len = 0;
    OGRPoint sl_pt(5.0, 6.0, 7.0);
    ASSERT_EQ(OGRERR_NONE,
              OGRSQLiteExportSpatiaLiteGeometry(&sl_pt, 4326, wkbNDR, false,
                                                false, &sl_data, &sl_len));
    ASSERT_NE(nullptr, sl_data);

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_prepare_v2(hDB, "INSERT INTO tbl_spatialite (geom) VALUES (?)",
                           -1, &stmt, nullptr));
    ASSERT_NE(nullptr, stmt);
    sqlite3_bind_blob(stmt, 1, sl_data, sl_len, CPLFree);
    EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
    sqlite3_finalize(stmt);

    // Call GetExtent3D with attribute filter so !m_soFilter.empty() (hits lines
    // 9103-9114)
    sl_layer->SetAttributeFilter("FID >= 1");
    OGREnvelope3D extent3D;
    EXPECT_EQ(OGRERR_NONE, sl_layer->GetExtent3D(0, &extent3D, true));
    EXPECT_NEAR(5.0, extent3D.MinX, 1e-4);
    sl_layer->SetAttributeFilter(nullptr);

    // Read via Arrow stream (hits lines 8000-8025)
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(sl_layer->GetArrowStream(&stream, nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(1, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // 6. Delete a feature and call GetFeatureCount on table with
  // gpkg_ogr_contents (hits lines 4179-4183)
  {
    EXPECT_EQ(OGRERR_NONE, layer->DeleteFeature(1));
    EXPECT_EQ(0, layer->GetFeatureCount(true));
  }

  // 7. Layer with SRID option < 32767 importing into gpkg_spatial_ref_sys (hits
  // lines 5749-5760)
  {
    const char* const srid_opts[] = {"SRID=32631", nullptr};
    OGRLayer* srid_layer = ds->CreateLayer("tbl_srid32631", nullptr, wkbPoint,
                                           const_cast<char**>(srid_opts));
    ASSERT_NE(nullptr, srid_layer);
  }

  // 8. Aspatial layer with 0 fields and empty feature with OGRNullFID (hits
  // lines 623-624, 693-694)
  {
    OGRLayer* aspatial_l =
        ds->CreateLayer("tbl_aspatial_zero", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, aspatial_l);
    OGRFeature* empty_f = OGRFeature::CreateFeature(aspatial_l->GetLayerDefn());
    empty_f->SetFID(OGRNullFID);
    EXPECT_EQ(OGRERR_NONE, aspatial_l->CreateFeature(empty_f));
    OGRFeature::DestroyFeature(empty_f);
  }

  // 9. Spatial layer without spatial index: spatial filter + attribute filter
  // (hits lines 5566-5573, 5622-5626)
  {
    const char* const no_spi_opts[] = {"SPATIAL_INDEX=NO", nullptr};
    OGRLayer* no_spi = ds->CreateLayer("tbl_no_spi", &srs, wkbPoint,
                                       const_cast<char**>(no_spi_opts));
    ASSERT_NE(nullptr, no_spi);
    OGRFeature* f_spi = OGRFeature::CreateFeature(no_spi->GetLayerDefn());
    OGRPoint pt_spi(5.0, 5.0);
    f_spi->SetGeometry(&pt_spi);
    ASSERT_EQ(OGRERR_NONE, no_spi->CreateFeature(f_spi));
    OGRFeature::DestroyFeature(f_spi);
    EXPECT_EQ(OGRERR_NONE, no_spi->SyncToDisk());

    // Spatial filter + attribute filter
    no_spi->SetSpatialFilterRect(0, 0, 10, 10);
    no_spi->SetAttributeFilter("FID >= 1");
    no_spi->ResetReading();
    OGRFeature* f_got = no_spi->GetNextFeature();
    ASSERT_NE(nullptr, f_got);
    OGRFeature::DestroyFeature(f_got);

    // Infinite spatial filter + attribute filter (hits lines 5566-5573)
    constexpr double kInf = std::numeric_limits<double>::infinity();
    no_spi->SetSpatialFilterRect(-kInf, -kInf, kInf, kInf);
    no_spi->ResetReading();
    f_got = no_spi->GetNextFeature();
    ASSERT_NE(nullptr, f_got);
    OGRFeature::DestroyFeature(f_got);
  }

  // 10. LAUNDER=YES with special characters in CreateField (hits lines
  // 1741-1743)
  {
    const char* const launder_opts[] = {"LAUNDER=YES", nullptr};
    OGRLayer* l_launder = ds->CreateLayer("tbl_launder", &srs, wkbPoint,
                                          const_cast<char**>(launder_opts));
    ASSERT_NE(nullptr, l_launder);
    OGRFieldDefn f_special("my field#1", OFTString);
    ASSERT_EQ(OGRERR_NONE, l_launder->CreateField(&f_special));
    EXPECT_STREQ("my_field_1",
                 l_launder->GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
  }
}

TEST_F(OGRGeoPackageTableLayerTest,
       AsyncRTreeArrowMemoryLimitsAndMetadataEdgeCases) {
  const std::string gpkg_path = "/vsimem/target6_ninety.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(gpkg_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  absl::Cleanup unlink_cleanup([&gpkg_path] { VSIUnlink(gpkg_path.c_str()); });

  auto* gpkg_ds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gpkg_ds);
  sqlite3* hDB = gpkg_ds->GetDB();
  ASSERT_NE(nullptr, hDB);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  // 1. InitAsyncRTree failure when ATTACH DATABASE fails (L2820-L2830)
  {
    CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", "YES");
    CPLSetConfigOption("OGR_GPKG_NUM_THREADS", "1");
    absl::Cleanup cleanup_opts([] {
      CPLSetConfigOption("OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE", nullptr);
      CPLSetConfigOption("OGR_GPKG_NUM_THREADS", nullptr);
    });

    const char* const spi_opts[] = {"SPATIAL_INDEX=YES", nullptr};
    OGRLayer* l_attach = ds->CreateLayer(
        "tbl_rtree_attach_fail", &srs, wkbPoint, const_cast<char**>(spi_opts));
    ASSERT_NE(nullptr, l_attach);
    ASSERT_EQ(OGRERR_NONE, l_attach->SyncToDisk());

    std::string attach_name =
        std::string("temp_rtree_") + CPLMD5String("tbl_rtree_attach_fail");
    std::string sql = "ATTACH DATABASE ':memory:' AS \"" + attach_name + "\"";
    sqlite3_exec(hDB, sql.c_str(), nullptr, nullptr, nullptr);

    OGRFeature f(l_attach->GetLayerDefn());
    OGRPoint pt(1, 2);
    f.SetGeometry(&pt);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPL_IGNORE_RET_VAL(l_attach->CreateFeature(&f));
    CPLPopErrorHandler();

    std::string detach_sql = "DETACH DATABASE \"" + attach_name + "\"";
    sqlite3_exec(hDB, detach_sql.c_str(), nullptr, nullptr, nullptr);
  }

  // 2. GetGeometryTypes and IGetExtent3D sqlite3_exec failures (L7807-L7813,
  // L9184-L9189)
  {
    OGRLayer* l_drop =
        ds->CreateLayer("tbl_drop_sql", &srs, wkbPoint25D, nullptr);
    ASSERT_NE(nullptr, l_drop);
    OGRFeature f(l_drop->GetLayerDefn());
    OGRPoint pt(1, 2, 3);
    f.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, l_drop->CreateFeature(&f));
    ASSERT_EQ(OGRERR_NONE, l_drop->SyncToDisk());

    sqlite3_exec(hDB, "DROP TABLE tbl_drop_sql", nullptr, nullptr, nullptr);

    int nEntryCount = 0;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRGeometryTypeCounter* counters =
        l_drop->GetGeometryTypes(0, 0, nEntryCount, nullptr, nullptr);
    CPLFree(counters);

    OGREnvelope3D env3d;
    EXPECT_NE(OGRERR_NONE, l_drop->GetExtent3D(0, &env3d, true));
    CPLPopErrorHandler();
  }

  // 3. Arrow oHeader.bExtentHasXY with spatial filter (L8041-L8046)
  {
    const char* const no_spi[] = {"SPATIAL_INDEX=NO", nullptr};
    OGRLayer* l_env = ds->CreateLayer("tbl_arrow_env", &srs, wkbLineString,
                                      const_cast<char**>(no_spi));
    ASSERT_NE(nullptr, l_env);
    OGRFeature f(l_env->GetLayerDefn());
    OGRLineString line;
    line.addPoint(0, 0);
    line.addPoint(10, 10);
    f.SetGeometry(&line);
    ASSERT_EQ(OGRERR_NONE, l_env->CreateFeature(&f));
    ASSERT_EQ(OGRERR_NONE, l_env->SyncToDisk());

    l_env->SetSpatialFilterRect(0, 0, 20, 20);
    struct ArrowArrayStream stream;
    if (l_env->GetArrowStream(&stream, nullptr)) {
      struct ArrowArray array;
      while (stream.get_next(&stream, &array) == 0 &&
             array.release != nullptr) {
        array.release(&array);
      }
      stream.release(&stream);
    }
    l_env->SetSpatialFilter(nullptr);
  }

  // 4. Arrow async worker sqlite3_exec error and subsequent get_next
  // (L8420-L8423, L8456-L8459)
  {
    CPLSetConfigOption("OGR_GPKG_NUM_THREADS", "0");
    absl::Cleanup cleanup(
        [] { CPLSetConfigOption("OGR_GPKG_NUM_THREADS", nullptr); });

    OGRLayer* l_err =
        ds->CreateLayer("tbl_arrow_async_err", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, l_err);
    OGRFeature f(l_err->GetLayerDefn());
    OGRPoint pt(5, 5);
    f.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, l_err->CreateFeature(&f));
    ASSERT_EQ(OGRERR_NONE, l_err->SyncToDisk());

    l_err->SetAttributeFilter("1=1");
    struct ArrowArrayStream stream;
    if (l_err->GetArrowStream(&stream, nullptr)) {
      sqlite3_exec(hDB, "DROP TABLE tbl_arrow_async_err", nullptr, nullptr,
                   nullptr);

      struct ArrowArray array;
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_NE(0, stream.get_next(&stream, &array));
      EXPECT_EQ(0, stream.get_next(&stream, &array));
      CPLPopErrorHandler();
      stream.release(&stream);
    }
  }

  // 5. Arrow OGR_ARROW_MEM_LIMIT and thread mismatch (L7868-L7878, L8066-L8074,
  // L8720-L8731, L8747-L8752)
  {
    OGRLayer* l_mem =
        ds->CreateLayer("tbl_arrow_mem", &srs, wkbLineString, nullptr);
    ASSERT_NE(nullptr, l_mem);
    OGRFieldDefn f_str("str", OFTString);
    ASSERT_EQ(OGRERR_NONE, l_mem->CreateField(&f_str));

    for (int i = 0; i < 6; ++i) {
      OGRFeature f(l_mem->GetLayerDefn());
      OGRLineString ls;
      ls.addPoint(0, 0);
      ls.addPoint(1, 1);
      ls.addPoint(2, 2);
      ls.addPoint(3, 3);
      f.SetGeometry(&ls);
      f.SetField(
          "str",
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
      ASSERT_EQ(OGRERR_NONE, l_mem->CreateFeature(&f));
    }
    ASSERT_EQ(OGRERR_NONE, l_mem->SyncToDisk());

    CPLSetConfigOption("OGR_ARROW_MEM_LIMIT", "90");
    absl::Cleanup mem_cleanup([] {
      CPLSetConfigOption("OGR_ARROW_MEM_LIMIT", nullptr);
      CPLSetConfigOption("OGR_GPKG_NUM_THREADS", nullptr);
    });

    // Case A: Async worker mode (attribute filter triggers asynchronous worker)
    {
      l_mem->SetAttributeFilter("1=1");
      struct ArrowArrayStream stream;
      if (l_mem->GetArrowStream(&stream, nullptr)) {
        struct ArrowArray array;
        while (stream.get_next(&stream, &array) == 0 &&
               array.release != nullptr) {
          array.release(&array);
        }
        stream.release(&stream);
      }
    }

    // Case B: Sync mode (no attribute filter, single thread)
    {
      l_mem->SetAttributeFilter(nullptr);
      CPLSetConfigOption("OGR_GPKG_NUM_THREADS", "1");
      struct ArrowArrayStream stream;
      if (l_mem->GetArrowStream(&stream, nullptr)) {
        struct ArrowArray array;
        while (stream.get_next(&stream, &array) == 0 &&
               array.release != nullptr) {
          array.release(&array);
        }
        stream.release(&stream);
      }
    }

    // Case C: Prefetch mode memory limit
    {
      l_mem->SetAttributeFilter(nullptr);
      CPLSetConfigOption("OGR_GPKG_NUM_THREADS", "2");
      struct ArrowArrayStream stream;
      if (l_mem->GetArrowStream(&stream, nullptr)) {
        struct ArrowArray array;
        while (stream.get_next(&stream, &array) == 0 &&
               array.release != nullptr) {
          array.release(&array);
        }
        stream.release(&stream);
      }
    }

    // Case D: Prefetch task->m_iStartShapeId != m_iNextShapeId
    {
      CPLSetConfigOption("OGR_GPKG_NUM_THREADS", "2");
      const char* const stream_opts[] = {"MAX_FEATURES_IN_BATCH=1", nullptr};
      struct ArrowArrayStream stream;
      l_mem->ResetReading();
      if (l_mem->GetArrowStream(&stream, const_cast<char**>(stream_opts))) {
        struct ArrowArray array;
        if (stream.get_next(&stream, &array) == 0 && array.release != nullptr) {
          array.release(&array);
          OGRFeature* f_adv = l_mem->GetNextFeature();
          OGRFeature::DestroyFeature(f_adv);
          CPLPushErrorHandler(CPLQuietErrorHandler);
          if (stream.get_next(&stream, &array) == 0 &&
              array.release != nullptr) {
            array.release(&array);
          }
          CPLPopErrorHandler();
        }
        stream.release(&stream);
      }
    }
  }

  // 6. ReadTableDefinition lazy metadata edge cases (L1008-L1010, L1015-L1018,
  // L1062-L1070, L1087-L1089, L1107-L1110)
  {
    const std::string multi_gpkg = "/vsimem/multi_layers_edge.gpkg";
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, drv);
    {
      auto multi_ds = std::unique_ptr<GDALDataset>(
          drv->Create(multi_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, multi_ds);
      for (int i = 0; i < 12; ++i) {
        std::string name = CPLSPrintf("t%d", i);
        OGRLayer* ly =
            multi_ds->CreateLayer(name.c_str(), &srs, wkbPoint, nullptr);
        ASSERT_NE(nullptr, ly);
        OGRFieldDefn f_uniq("u", OFTInteger);
        f_uniq.SetUnique(true);
        CPL_IGNORE_RET_VAL(ly->CreateField(&f_uniq));
        OGRFeature f(ly->GetLayerDefn());
        OGRPoint pt(i, i);
        f.SetGeometry(&pt);
        f.SetField("u", i);
        ASSERT_EQ(OGRERR_NONE, ly->CreateFeature(&f));
        ASSERT_EQ(OGRERR_NONE, ly->SyncToDisk());
      }
    }

    auto reopened_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(multi_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, reopened_ds);
    auto* gds = dynamic_cast<GDALGeoPackageDataset*>(reopened_ds.get());
    ASSERT_NE(nullptr, gds);
    sqlite3* m_hDB = gds->GetDB();

    sqlite3_exec(m_hDB,
                 "DELETE FROM gpkg_geometry_columns WHERE table_name = 't0'",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(m_hDB,
                 "UPDATE gpkg_geometry_columns SET column_name = 'wrong_col' "
                 "WHERE table_name = 't1'",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(m_hDB, "DROP TABLE t2", nullptr, nullptr, nullptr);
    sqlite3_exec(m_hDB,
                 "UPDATE gpkg_ogr_contents SET feature_count = "
                 "9223372036854775807 WHERE table_name = 't3'",
                 nullptr, nullptr, nullptr);

    CPLPushErrorHandler(CPLQuietErrorHandler);
    for (int i = 0; i < reopened_ds->GetLayerCount(); ++i) {
      OGRLayer* ly = reopened_ds->GetLayer(i);
      if (ly) {
        ly->GetLayerDefn();
      }
    }
    CPLPopErrorHandler();

    OGRLayer* l_t3 = reopened_ds->GetLayerByName("t3");
    if (l_t3) {
      OGRFeature f(l_t3->GetLayerDefn());
      OGRPoint pt(100, 100);
      f.SetGeometry(&pt);
      f.SetField("u", 9999);
      EXPECT_EQ(OGRERR_NONE, l_t3->CreateFeature(&f));
      EXPECT_EQ(2, l_t3->GetFeatureCount(true));
    }

    reopened_ds.reset();
    VSIUnlink(multi_gpkg.c_str());
  }

  // 7. GetColumnsOfCreateTable date defaults during DeleteField (L5967-L5970)
  {
    OGRLayer* l_dt =
        ds->CreateLayer("tbl_recreate_dt", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, l_dt);
    OGRFieldDefn f_dt1("dt1", OFTDateTime);
    f_dt1.SetDefault("'2023-05-01 12:00:00'");
    ASSERT_EQ(OGRERR_NONE, l_dt->CreateField(&f_dt1));
    OGRFieldDefn f_dt2("dt2", OFTDateTime);
    f_dt2.SetDefault("CURRENT_TIMESTAMP");
    ASSERT_EQ(OGRERR_NONE, l_dt->CreateField(&f_dt2));
    OGRFieldDefn f_del("to_del", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, l_dt->CreateField(&f_del));

    OGRFeature f(l_dt->GetLayerDefn());
    OGRPoint pt(1, 1);
    f.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, l_dt->CreateFeature(&f));
    ASSERT_EQ(OGRERR_NONE, l_dt->SyncToDisk());

    int del_idx = l_dt->GetLayerDefn()->GetFieldIndex("to_del");
    ASSERT_GE(del_idx, 0);
    EXPECT_EQ(OGRERR_NONE, l_dt->DeleteField(del_idx));
  }
}

// TODO: b/335317901 - Cover remaining SQLite prepare/step error branches
// in ogrgeopackagetablelayer.cpp.

}  // namespace
}  // namespace autotest2
