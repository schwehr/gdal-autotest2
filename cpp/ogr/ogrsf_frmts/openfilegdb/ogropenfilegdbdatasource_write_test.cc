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
// Tests Open FileGDB datasource write functionality
// (ogropenfilegdbdatasource_write.cpp).

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_relationship.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogr_openfilegdb.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern "C" void RegisterOGROpenFileGDB();

namespace autotest2 {
namespace {

class OGROpenFileGDBDataSourceWriteTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGROpenFileGDB(); }

  std::unique_ptr<GDALDataset> CreateDataset(const std::string& path) {
    auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return nullptr;
    return std::unique_ptr<GDALDataset>(
        driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  }
};

TEST_F(OGROpenFileGDBDataSourceWriteTest,
       CreateDatasetAndLayersAndSpatialRefs) {
  const std::string dir = "/vsimem/test_ds_write_create.gdb";
  auto cleanup = absl::MakeCleanup([&dir] { VSIRmdirRecursive(dir.c_str()); });

  // Create error: non .gdb extension
  {
    OGROpenFileGDBDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Create("/vsimem/bad_ext.txt"));
  }

  // Create error: already existing directory
  {
    const std::string exists_dir = "/vsimem/already_exists.gdb";
    VSIMkdir(exists_dir.c_str(), 0755);
    auto c = absl::MakeCleanup(
        [&exists_dir] { VSIRmdirRecursive(exists_dir.c_str()); });
    OGROpenFileGDBDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Create(exists_dir.c_str()));
  }

  // Create error: directory already exists without extension
  {
    const std::string bare_exists = "/vsimem/bare_exists";
    VSIMkdir(bare_exists.c_str(), 0755);
    auto c = absl::MakeCleanup(
        [&bare_exists] { VSIRmdirRecursive(bare_exists.c_str()); });
    OGROpenFileGDBDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Create(bare_exists.c_str()));
  }

  // ICreateLayer error: read-only dataset (uninitialized default access)
  {
    OGROpenFileGDBDataSource uninit_ds;
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, uninit_ds.ICreateLayer("fail_layer", nullptr, nullptr));
  }

  // Valid dataset creation
  auto ds = CreateDataset(dir);
  ASSERT_NE(ds, nullptr);
  auto* gdb_ds = dynamic_cast<OGROpenFileGDBDataSource*>(ds.get());
  ASSERT_NE(gdb_ds, nullptr);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  // Layer 1: PointZM with EPSG:4326 -> calls AddNewSpatialRef
  OGRLayer* lyr1 = ds->CreateLayer("points_1", &srs, wkbPointZM, nullptr);
  ASSERT_NE(lyr1, nullptr);

  // Layer 2: PointZM with same EPSG:4326 -> calls GetExistingSpatialRef
  // (returns true)
  OGRLayer* lyr2 = ds->CreateLayer("points_2", &srs, wkbPointZM, nullptr);
  ASSERT_NE(lyr2, nullptr);

  // Layer 3: LineString promoted to MultiLineString, with feature dataset &
  // documentation
  const char* const fds_options[] = {
      "FEATURE_DATASET=roads_dataset",
      "DOCUMENTATION=<doc>Roads documentation</doc>", nullptr};
  OGRLayer* lyr3 = ds->CreateLayer("roads", &srs, wkbLineString,
                                   const_cast<char**>(fds_options));
  ASSERT_NE(lyr3, nullptr);
  EXPECT_EQ(lyr3->GetGeomType(), wkbMultiLineString);

  // Layer 4: Polygon promoted to MultiPolygon, reusing existing feature dataset
  const char* const fds_options2[] = {"FEATURE_DATASET=roads_dataset", nullptr};
  OGRLayer* lyr4 = ds->CreateLayer("parcels", &srs, wkbPolygon,
                                   const_cast<char**>(fds_options2));
  ASSERT_NE(lyr4, nullptr);
  EXPECT_EQ(lyr4->GetGeomType(), wkbMultiPolygon);

  // Layer 5: Aspatial table with documentation
  const char* const aspatial_options[] = {
      "DOCUMENTATION=<doc>Aspatial table documentation</doc>", nullptr};
  OGRLayer* lyr5 = ds->CreateLayer("non_spatial_tbl", nullptr, wkbNone,
                                   const_cast<char**>(aspatial_options));
  ASSERT_NE(lyr5, nullptr);

  // Flush so layers are registered into GDB_Items
  EXPECT_EQ(CE_None, ds->FlushCache(false));

  // Direct helper method tests
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(gdb_ds->UpdateXMLDefinition("nonexistent_layer", "<xml/>"));
  }
  EXPECT_TRUE(gdb_ds->UpdateXMLDefinition("points_1", "<DEFeatureClassInfo/>"));

  std::string uuid;
  EXPECT_FALSE(gdb_ds->FindUUIDFromName("nonexistent_item", uuid));
  EXPECT_TRUE(gdb_ds->FindUUIDFromName("points_1", uuid));
  EXPECT_FALSE(uuid.empty());

  // Test FlushCache on read-write dataset with modified layer
  OGRFeatureDefn* poFDefn = lyr1->GetLayerDefn();
  OGRFeature* poFeature = OGRFeature::CreateFeature(poFDefn);
  OGRPoint pt(10.0, 20.0, 5.0, 1.0);
  poFeature->SetGeometry(&pt);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(poFeature));
  OGRFeature::DestroyFeature(poFeature);

  EXPECT_EQ(CE_None, ds->FlushCache(false));

  // Close dataset and re-open read-only
  ds.reset();

  auto ro_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(dir.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(ro_ds, nullptr);
  auto* gdb_ro = dynamic_cast<OGROpenFileGDBDataSource*>(ro_ds.get());
  ASSERT_NE(gdb_ro, nullptr);

  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, ro_ds->CreateLayer("fail", nullptr, wkbPoint, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, ro_ds->DeleteLayer(0));
  }
  EXPECT_EQ(CE_None, ro_ds->FlushCache(false));
}

TEST_F(OGROpenFileGDBDataSourceWriteTest, FieldDomainsAndDomainLinking) {
  const std::string dir = "/vsimem/test_ds_write_domains.gdb";
  auto cleanup = absl::MakeCleanup([&dir] { VSIRmdirRecursive(dir.c_str()); });

  auto ds = CreateDataset(dir);
  ASSERT_NE(ds, nullptr);
  auto* gdb_ds = dynamic_cast<OGROpenFileGDBDataSource*>(ds.get());
  ASSERT_NE(gdb_ds, nullptr);

  std::string failureReason;

  // AddFieldDomain: Coded domain
  std::vector<OGRCodedValue> codedValues;
  codedValues.push_back({CPLStrdup("1"), CPLStrdup("Residential")});
  codedValues.push_back({CPLStrdup("2"), CPLStrdup("Commercial")});
  codedValues.push_back({nullptr, nullptr});
  auto coded_domain = std::make_unique<OGRCodedFieldDomain>(
      "landuse_dom", "Land use domain", OFTInteger, OFSTNone,
      std::move(codedValues));

  EXPECT_TRUE(ds->AddFieldDomain(std::move(coded_domain), failureReason));

  // Add duplicate domain should fail
  std::vector<OGRCodedValue> dupValues;
  dupValues.push_back({nullptr, nullptr});
  auto dup_domain = std::make_unique<OGRCodedFieldDomain>(
      "landuse_dom", "Duplicate domain", OFTInteger, OFSTNone,
      std::move(dupValues));
  EXPECT_FALSE(ds->AddFieldDomain(std::move(dup_domain), failureReason));

  // Add Range domain
  OGRField sMin;
  sMin.Real = 0.0;
  OGRField sMax;
  sMax.Real = 100.0;
  auto range_domain = std::make_unique<OGRRangeFieldDomain>(
      "speed_dom", "Speed limit domain", OFTReal, OFSTNone, sMin, true, sMax,
      true);
  EXPECT_TRUE(ds->AddFieldDomain(std::move(range_domain), failureReason));

  // Create a layer with fields linking to these domains
  OGRLayer* lyr = ds->CreateLayer("parcels", nullptr, wkbPoint, nullptr);
  ASSERT_NE(lyr, nullptr);

  OGRFieldDefn fld1("landuse", OFTInteger);
  fld1.SetDomainName("landuse_dom");
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld1));

  OGRFieldDefn fld2("speed", OFTReal);
  fld2.SetDomainName("speed_dom");
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld2));

  // Flush so parcels layer is registered into GDB_Items
  EXPECT_EQ(CE_None, ds->FlushCache(false));

  // Test LinkDomainToTable & UnlinkDomainToTable direct calls
  std::string lyr_uuid;
  EXPECT_TRUE(gdb_ds->FindUUIDFromName("parcels", lyr_uuid));

  // Calling LinkDomainToTable again returns true (already linked)
  EXPECT_TRUE(gdb_ds->LinkDomainToTable("landuse_dom", lyr_uuid));
  // Linking nonexistent domain returns false
  EXPECT_FALSE(gdb_ds->LinkDomainToTable("nonexistent_dom", lyr_uuid));

  // Unlinking nonexistent domain returns false
  EXPECT_FALSE(gdb_ds->UnlinkDomainToTable("nonexistent_dom", lyr_uuid));
  // Unlink speed_dom returns true
  EXPECT_TRUE(gdb_ds->UnlinkDomainToTable("speed_dom", lyr_uuid));

  // UpdateFieldDomain
  {
    auto no_such = std::make_unique<OGRCodedFieldDomain>(
        "no_such_dom", "", OFTInteger, OFSTNone, std::vector<OGRCodedValue>{});
    EXPECT_FALSE(ds->UpdateFieldDomain(std::move(no_such), failureReason));

    // Update landuse_dom
    std::vector<OGRCodedValue> updatedValues;
    updatedValues.push_back({CPLStrdup("1"), CPLStrdup("Residential New")});
    updatedValues.push_back({CPLStrdup("3"), CPLStrdup("Industrial")});
    updatedValues.push_back({nullptr, nullptr});
    auto updated_coded = std::make_unique<OGRCodedFieldDomain>(
        "landuse_dom", "Updated landuse", OFTInteger, OFSTNone,
        std::move(updatedValues));
    EXPECT_TRUE(ds->UpdateFieldDomain(std::move(updated_coded), failureReason));

    // Update speed_dom
    OGRField newMin;
    newMin.Real = 10.0;
    OGRField newMax;
    newMax.Real = 120.0;
    auto updated_range = std::make_unique<OGRRangeFieldDomain>(
        "speed_dom", "Updated speed", OFTReal, OFSTNone, newMin, true, newMax,
        true);
    EXPECT_TRUE(ds->UpdateFieldDomain(std::move(updated_range), failureReason));
  }

  // DeleteFieldDomain
  EXPECT_FALSE(ds->DeleteFieldDomain("no_such_dom", failureReason));

  // Delete landuse_dom while still linked to parcels: cleans up relationships
  // and field domain
  EXPECT_TRUE(ds->DeleteFieldDomain("landuse_dom", failureReason));
  EXPECT_TRUE(ds->DeleteFieldDomain("speed_dom", failureReason));

  // Test read-only failures
  ds.reset();
  auto ro_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(dir.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(ro_ds, nullptr);

  {
    WithQuietHandler quiet;
    auto d1 = std::make_unique<OGRCodedFieldDomain>(
        "d1", "", OFTInteger, OFSTNone, std::vector<OGRCodedValue>{});
    EXPECT_FALSE(ro_ds->AddFieldDomain(std::move(d1), failureReason));

    auto d2 = std::make_unique<OGRCodedFieldDomain>(
        "d2", "", OFTInteger, OFSTNone, std::vector<OGRCodedValue>{});
    EXPECT_FALSE(ro_ds->UpdateFieldDomain(std::move(d2), failureReason));

    EXPECT_FALSE(ro_ds->DeleteFieldDomain("d3", failureReason));
  }
}

TEST_F(OGROpenFileGDBDataSourceWriteTest, RelationshipsLifecycle) {
  const std::string dir = "/vsimem/test_ds_write_relationships.gdb";
  auto cleanup = absl::MakeCleanup([&dir] { VSIRmdirRecursive(dir.c_str()); });

  auto ds = CreateDataset(dir);
  ASSERT_NE(ds, nullptr);
  auto* gdb_ds = dynamic_cast<OGROpenFileGDBDataSource*>(ds.get());
  ASSERT_NE(gdb_ds, nullptr);

  // Create tables for relationships
  auto make_table = [&](const char* name, const char* fld_name) {
    OGRLayer* lyr = ds->CreateLayer(name, nullptr, wkbNone, nullptr);
    OGRFieldDefn fld(fld_name, OFTInteger);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
    return lyr;
  };

  ASSERT_NE(make_table("origin_tbl", "orig_id"), nullptr);
  ASSERT_NE(make_table("dest_tbl", "dest_fk"), nullptr);
  ASSERT_NE(make_table("origin_tbl2", "orig_id"), nullptr);
  ASSERT_NE(make_table("dest_tbl2", "dest_fk"), nullptr);

  // Mapping table for explicit M-to-N
  OGRLayer* map_lyr =
      ds->CreateLayer("map_explicit", nullptr, wkbNone, nullptr);
  ASSERT_NE(map_lyr, nullptr);
  OGRFieldDefn fld_m1("m_orig_fk", OFTString);
  EXPECT_EQ(OGRERR_NONE, map_lyr->CreateField(&fld_m1));
  OGRFieldDefn fld_m2("m_dest_fk", OFTString);
  EXPECT_EQ(OGRERR_NONE, map_lyr->CreateField(&fld_m2));

  std::string failureReason;

  // AddRelationship errors
  // 1. GRC_MANY_TO_ONE
  auto rel_m_1 = std::make_unique<GDALRelationship>(
      "rel_m_1", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_ONE);
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_m_1), failureReason));

  // 2. GRC_MANY_TO_MANY with mismatched mapping table name
  auto rel_mn_bad = std::make_unique<GDALRelationship>(
      "rel_mn_bad", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_MANY);
  rel_mn_bad->SetMappingTableName("other_map_table");
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_mn_bad), failureReason));

  // 3. Missing left table
  auto rel_bad_l = std::make_unique<GDALRelationship>(
      "rel_bad_l", "missing_left", "dest_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  rel_bad_l->SetLeftTableFields({"orig_id"});
  rel_bad_l->SetRightTableFields({"dest_fk"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_bad_l), failureReason));

  // 4. Missing right table
  auto rel_bad_r = std::make_unique<GDALRelationship>(
      "rel_bad_r", "origin_tbl", "missing_right",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  rel_bad_r->SetLeftTableFields({"orig_id"});
  rel_bad_r->SetRightTableFields({"dest_fk"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_bad_r), failureReason));

  // Valid 1: GRC_ONE_TO_ONE
  auto rel_1_1 = std::make_unique<GDALRelationship>(
      "rel_1_1", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  rel_1_1->SetLeftTableFields({"orig_id"});
  rel_1_1->SetRightTableFields({"dest_fk"});
  rel_1_1->SetForwardPathLabel("forward_1_1");
  rel_1_1->SetBackwardPathLabel("backward_1_1");
  EXPECT_TRUE(ds->AddRelationship(std::move(rel_1_1), failureReason));

  // Duplicate name fails
  auto rel_dup = std::make_unique<GDALRelationship>(
      "rel_1_1", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_dup), failureReason));

  // Valid 2: GRC_ONE_TO_MANY
  auto rel_1_n = std::make_unique<GDALRelationship>(
      "rel_1_n", "origin_tbl2", "dest_tbl2",
      GDALRelationshipCardinality::GRC_ONE_TO_MANY);
  rel_1_n->SetLeftTableFields({"orig_id"});
  rel_1_n->SetRightTableFields({"dest_fk"});
  rel_1_n->SetForwardPathLabel("forward_1_n");
  rel_1_n->SetBackwardPathLabel("backward_1_n");
  EXPECT_TRUE(ds->AddRelationship(std::move(rel_1_n), failureReason));

  // Valid 3: GRC_MANY_TO_MANY with explicit mapping table
  auto rel_mn_exp = std::make_unique<GDALRelationship>(
      "map_explicit", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_MANY);
  rel_mn_exp->SetMappingTableName("map_explicit");
  rel_mn_exp->SetLeftTableFields({"orig_id"});
  rel_mn_exp->SetRightTableFields({"dest_fk"});
  rel_mn_exp->SetLeftMappingTableFields({"m_orig_fk"});
  rel_mn_exp->SetRightMappingTableFields({"m_dest_fk"});
  EXPECT_TRUE(ds->AddRelationship(std::move(rel_mn_exp), failureReason));

  // Valid 4: GRC_MANY_TO_MANY with empty mapping table name (auto creates
  // table)
  auto rel_mn_auto = std::make_unique<GDALRelationship>(
      "rel_mn_auto", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_MANY);
  rel_mn_auto->SetLeftTableFields({"orig_id"});
  rel_mn_auto->SetRightTableFields({"dest_fk"});
  rel_mn_auto->SetLeftMappingTableFields({"origin_fk"});
  rel_mn_auto->SetRightMappingTableFields({"destination_fk"});
  EXPECT_TRUE(ds->AddRelationship(std::move(rel_mn_auto), failureReason));

  // Verify GetRelationshipNames and GetRelationship
  auto names = ds->GetRelationshipNames(nullptr);
  EXPECT_THAT(names, ::testing::UnorderedElementsAre(
                         "rel_1_1", "rel_1_n", "map_explicit", "rel_mn_auto"));
  EXPECT_NE(ds->GetRelationship("rel_1_1"), nullptr);
  EXPECT_EQ(ds->GetRelationship("nonexistent"), nullptr);

  // UpdateRelationship tests
  // Error: non-existent relationship
  auto upd_nonexistent = std::make_unique<GDALRelationship>(
      "no_such_rel", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  EXPECT_FALSE(
      ds->UpdateRelationship(std::move(upd_nonexistent), failureReason));

  // Error: GRC_MANY_TO_ONE
  auto upd_m_1 = std::make_unique<GDALRelationship>(
      "rel_1_1", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_ONE);
  EXPECT_FALSE(ds->UpdateRelationship(std::move(upd_m_1), failureReason));

  // Error: missing left table
  auto upd_bad_l = std::make_unique<GDALRelationship>(
      "rel_1_1", "missing_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  EXPECT_FALSE(ds->UpdateRelationship(std::move(upd_bad_l), failureReason));

  // Error: missing right table
  auto upd_bad_r = std::make_unique<GDALRelationship>(
      "rel_1_1", "origin_tbl", "missing_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  EXPECT_FALSE(ds->UpdateRelationship(std::move(upd_bad_r), failureReason));

  // Error: GRC_MANY_TO_MANY with non-existent mapping table
  auto upd_bad_map = std::make_unique<GDALRelationship>(
      "rel_1_1", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_MANY);
  upd_bad_map->SetMappingTableName("nonexistent_mapping_table");
  EXPECT_FALSE(ds->UpdateRelationship(std::move(upd_bad_map), failureReason));

  // Valid Update 1: update rel_1_1 (change labels / cardinality to 1-to-many)
  auto upd_1_1 = std::make_unique<GDALRelationship>(
      "rel_1_1", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_ONE_TO_MANY);
  upd_1_1->SetLeftTableFields({"orig_id"});
  upd_1_1->SetRightTableFields({"dest_fk"});
  upd_1_1->SetForwardPathLabel("new_forward");
  upd_1_1->SetBackwardPathLabel("new_backward");
  EXPECT_TRUE(ds->UpdateRelationship(std::move(upd_1_1), failureReason));

  // Valid Update 2: update rel_1_n (change cardinality to 1-to-1)
  auto upd_1_n = std::make_unique<GDALRelationship>(
      "rel_1_n", "origin_tbl2", "dest_tbl2",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  upd_1_n->SetLeftTableFields({"orig_id"});
  upd_1_n->SetRightTableFields({"dest_fk"});
  EXPECT_TRUE(ds->UpdateRelationship(std::move(upd_1_n), failureReason));

  // Valid Update 3: update map_explicit (many-to-many)
  auto upd_mn = std::make_unique<GDALRelationship>(
      "map_explicit", "origin_tbl", "dest_tbl",
      GDALRelationshipCardinality::GRC_MANY_TO_MANY);
  upd_mn->SetMappingTableName("map_explicit");
  upd_mn->SetLeftTableFields({"orig_id"});
  upd_mn->SetRightTableFields({"dest_fk"});
  upd_mn->SetLeftMappingTableFields({"m_orig_fk"});
  upd_mn->SetRightMappingTableFields({"m_dest_fk"});
  EXPECT_TRUE(ds->UpdateRelationship(std::move(upd_mn), failureReason));

  // DeleteRelationship
  EXPECT_FALSE(ds->DeleteRelationship("no_such_rel", failureReason));

  // Also exercise RemoveRelationshipFromItemRelationships where DestID matches
  std::string custom_guid = OFGDBGenerateUUID();
  std::string fake_origin = OFGDBGenerateUUID();
  gdb_ds->RegisterInItemRelationships(fake_origin, custom_guid,
                                      pszDatasetsRelatedThroughUUID);
  EXPECT_TRUE(gdb_ds->RemoveRelationshipFromItemRelationships(custom_guid));

  // Also exercise RemoveRelationshipFromItemRelationships where OriginID
  // matches
  std::string custom_guid2 = OFGDBGenerateUUID();
  std::string fake_dest = OFGDBGenerateUUID();
  gdb_ds->RegisterInItemRelationships(custom_guid2, fake_dest,
                                      pszDatasetsRelatedThroughUUID);
  EXPECT_TRUE(gdb_ds->RemoveRelationshipFromItemRelationships(custom_guid2));

  // Delete rel_1_1
  EXPECT_TRUE(ds->DeleteRelationship("rel_1_1", failureReason));
  EXPECT_EQ(ds->GetRelationship("rel_1_1"), nullptr);

  // DeleteLayer tests
  // Invalid layer indices
  EXPECT_EQ(OGRERR_FAILURE, ds->DeleteLayer(-1));
  EXPECT_EQ(OGRERR_FAILURE, ds->DeleteLayer(1000));

  // Delete origin_tbl2 (which is OriginID in rel_1_n)
  int idx_origin2 = -1;
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    if (std::string(ds->GetLayer(i)->GetName()) == "origin_tbl2") {
      idx_origin2 = i;
      break;
    }
  }
  ASSERT_GE(idx_origin2, 0);
  EXPECT_EQ(OGRERR_NONE, ds->DeleteLayer(idx_origin2));

  // Delete dest_tbl2 (which is DestID in rel_1_n)
  int idx_dest2 = -1;
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    if (std::string(ds->GetLayer(i)->GetName()) == "dest_tbl2") {
      idx_dest2 = i;
      break;
    }
  }
  ASSERT_GE(idx_dest2, 0);
  EXPECT_EQ(OGRERR_NONE, ds->DeleteLayer(idx_dest2));

  // Read-only tests for relationships
  ds.reset();
  auto ro_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(dir.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(ro_ds, nullptr);

  {
    WithQuietHandler quiet;
    auto r1 = std::make_unique<GDALRelationship>(
        "r1", "origin_tbl", "dest_tbl",
        GDALRelationshipCardinality::GRC_ONE_TO_ONE);
    EXPECT_FALSE(ro_ds->AddRelationship(std::move(r1), failureReason));

    auto r2 = std::make_unique<GDALRelationship>(
        "r2", "origin_tbl", "dest_tbl",
        GDALRelationshipCardinality::GRC_ONE_TO_ONE);
    EXPECT_FALSE(ro_ds->UpdateRelationship(std::move(r2), failureReason));

    EXPECT_FALSE(ro_ds->DeleteRelationship("map_explicit", failureReason));
  }
}

TEST_F(OGROpenFileGDBDataSourceWriteTest, TransactionsCommitAndRollback) {
  const std::string dir = "/vsimem/test_ds_write_transactions.gdb";
  auto cleanup = absl::MakeCleanup([&dir] { VSIRmdirRecursive(dir.c_str()); });

  auto ds = CreateDataset(dir);
  ASSERT_NE(ds, nullptr);
  auto* gdb_ds = dynamic_cast<OGROpenFileGDBDataSource*>(ds.get());
  ASSERT_NE(gdb_ds, nullptr);

  // Create initial layers
  OGRLayer* keep_lyr = ds->CreateLayer("keep_lyr", nullptr, wkbPoint, nullptr);
  ASSERT_NE(keep_lyr, nullptr);
  OGRFieldDefn fld1("val", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, keep_lyr->CreateField(&fld1));

  OGRFeature* feat1 = OGRFeature::CreateFeature(keep_lyr->GetLayerDefn());
  feat1->SetField(0, 100);
  OGRPoint pt1(1.0, 2.0);
  feat1->SetGeometry(&pt1);
  EXPECT_EQ(OGRERR_NONE, keep_lyr->CreateFeature(feat1));
  OGRFeature::DestroyFeature(feat1);

  OGRLayer* del_lyr =
      ds->CreateLayer("del_in_tx_lyr", nullptr, wkbPoint, nullptr);
  ASSERT_NE(del_lyr, nullptr);
  OGRFeature* feat2 = OGRFeature::CreateFeature(del_lyr->GetLayerDefn());
  OGRPoint pt2(3.0, 4.0);
  feat2->SetGeometry(&pt2);
  EXPECT_EQ(OGRERR_NONE, del_lyr->CreateFeature(feat2));
  OGRFeature::DestroyFeature(feat2);

  // Build spatial index on keep_lyr
  OGRLayer* sql_res =
      ds->ExecuteSQL("CREATE SPATIAL INDEX ON keep_lyr", nullptr, nullptr);
  if (sql_res) ds->ReleaseResultSet(sql_res);

  // Error cases before transaction
  {
    WithQuietHandler quiet;
    // StartTransaction with bForce=false
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, ds->StartTransaction(false));

    // Commit and Rollback without active transaction
    EXPECT_EQ(OGRERR_FAILURE, ds->CommitTransaction());
    EXPECT_EQ(OGRERR_FAILURE, ds->RollbackTransaction());
  }

  // Pre-existing backup directory error
  std::string backup_dir =
      CPLFormFilenameSafe(dir.c_str(), ".ogrtransaction_backup", nullptr);
  VSIMkdir(backup_dir.c_str(), 0755);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, ds->StartTransaction(true));
  }
  VSIRmdirRecursive(backup_dir.c_str());

  // === TRANSACTION 1: Commit ===
  EXPECT_EQ(OGRERR_NONE, ds->StartTransaction(true));
  EXPECT_TRUE(gdb_ds->IsInTransaction());

  // StartTransaction while already in transaction fails
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, ds->StartTransaction(true));
  }

  // Create two layers inside transaction
  OGRLayer* tx1_kept = ds->CreateLayer("tx1_kept", nullptr, wkbPoint, nullptr);
  ASSERT_NE(tx1_kept, nullptr);

  OGRLayer* tx1_del = ds->CreateLayer("tx1_del", nullptr, wkbPoint, nullptr);
  ASSERT_NE(tx1_del, nullptr);

  // Delete tx1_del while still in transaction (exercises
  // m_oSetLayersCreatedInTransaction removal)
  int tx1_del_idx = -1;
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    if (std::string(ds->GetLayer(i)->GetName()) == "tx1_del") {
      tx1_del_idx = i;
      break;
    }
  }
  ASSERT_GE(tx1_del_idx, 0);
  EXPECT_EQ(OGRERR_NONE, ds->DeleteLayer(tx1_del_idx));

  // Add field domain inside transaction
  std::string reason;
  auto tx1_dom = std::make_unique<OGRCodedFieldDomain>(
      "tx1_dom", "TX1 domain", OFTInteger, OFSTNone,
      std::vector<OGRCodedValue>{{CPLStrdup("1"), CPLStrdup("One")},
                                 {nullptr, nullptr}});
  EXPECT_TRUE(ds->AddFieldDomain(std::move(tx1_dom), reason));

  auto tx1_dom_upd = std::make_unique<OGRCodedFieldDomain>(
      "tx1_dom", "TX1 domain updated", OFTInteger, OFSTNone,
      std::vector<OGRCodedValue>{{CPLStrdup("2"), CPLStrdup("Two")},
                                 {nullptr, nullptr}});
  EXPECT_TRUE(ds->UpdateFieldDomain(std::move(tx1_dom_upd), reason));
  EXPECT_TRUE(ds->DeleteFieldDomain("tx1_dom", reason));

  // Add, update, and delete relationship inside transaction
  auto tx1_rel = std::make_unique<GDALRelationship>(
      "tx1_rel", "keep_lyr", "tx1_kept",
      GDALRelationshipCardinality::GRC_ONE_TO_ONE);
  tx1_rel->SetLeftTableFields({"val"});
  tx1_rel->SetRightTableFields({"FID"});
  EXPECT_TRUE(ds->AddRelationship(std::move(tx1_rel), reason));

  auto tx1_rel_upd = std::make_unique<GDALRelationship>(
      "tx1_rel", "keep_lyr", "tx1_kept",
      GDALRelationshipCardinality::GRC_ONE_TO_MANY);
  tx1_rel_upd->SetLeftTableFields({"val"});
  tx1_rel_upd->SetRightTableFields({"FID"});
  EXPECT_TRUE(ds->UpdateRelationship(std::move(tx1_rel_upd), reason));
  EXPECT_TRUE(ds->DeleteRelationship("tx1_rel", reason));

  // Modify keep_lyr inside transaction
  OGRFeature* feat3 = OGRFeature::CreateFeature(keep_lyr->GetLayerDefn());
  feat3->SetField(0, 200);
  OGRPoint pt3(5.0, 6.0);
  feat3->SetGeometry(&pt3);
  EXPECT_EQ(OGRERR_NONE, keep_lyr->CreateFeature(feat3));
  OGRFeature::DestroyFeature(feat3);

  // Commit transaction
  EXPECT_EQ(OGRERR_NONE, ds->CommitTransaction());
  EXPECT_FALSE(gdb_ds->IsInTransaction());

  // === TRANSACTION 2: Rollback ===
  EXPECT_EQ(OGRERR_NONE, ds->StartTransaction(true));
  EXPECT_TRUE(gdb_ds->IsInTransaction());

  // Create new layer in transaction
  OGRLayer* tx2_new =
      ds->CreateLayer("tx2_ephemeral", nullptr, wkbPoint, nullptr);
  ASSERT_NE(tx2_new, nullptr);

  // Delete pre-existing layer del_in_tx_lyr in transaction (exercises
  // m_oSetLayersDeletedInTransaction)
  int del_idx = -1;
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    if (std::string(ds->GetLayer(i)->GetName()) == "del_in_tx_lyr") {
      del_idx = i;
      break;
    }
  }
  ASSERT_GE(del_idx, 0);
  EXPECT_EQ(OGRERR_NONE, ds->DeleteLayer(del_idx));

  // Modify keep_lyr
  OGRFieldDefn fld_extra("extra_fld", OFTString);
  EXPECT_EQ(OGRERR_NONE, keep_lyr->CreateField(&fld_extra));

  // Rollback transaction
  EXPECT_EQ(OGRERR_NONE, ds->RollbackTransaction());
  EXPECT_FALSE(gdb_ds->IsInTransaction());

  // Re-open dataset to verify disk state was cleanly rolled back
  ds.reset();
  ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(dir.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(ds, nullptr);

  EXPECT_NE(ds->GetLayerByName("keep_lyr"), nullptr);
  EXPECT_NE(ds->GetLayerByName("del_in_tx_lyr"), nullptr);
  EXPECT_EQ(ds->GetLayerByName("tx2_ephemeral"), nullptr);

  // === TRANSACTION 3: Rollback error when backup directory is missing ===
  EXPECT_EQ(OGRERR_NONE, ds->StartTransaction(true));

  // Trigger system table backup
  OGRLayer* tx3_lyr = ds->CreateLayer("tx3_lyr", nullptr, wkbPoint, nullptr);
  ASSERT_NE(tx3_lyr, nullptr);

  // Manually delete the backup directory
  VSIRmdirRecursive(backup_dir.c_str());

  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, ds->RollbackTransaction());
  }

  // Read-only StartTransaction failure
  ds.reset();
  auto ro_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(dir.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(ro_ds, nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, ro_ds->StartTransaction(true));
  }
}

}  // namespace
}  // namespace autotest2
