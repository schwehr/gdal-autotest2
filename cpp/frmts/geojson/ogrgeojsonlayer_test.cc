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

// Tests for the GeoJSON layer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsonlayer.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonlayer.cpp

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonreader.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgeojsonlayer_test";

class OGRGeoJSONLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRGeoJSONLayerTest, StaticAndBasicAccessors) {
  EXPECT_STREQ(OGRGeoJSONLayer::GetValidLayerName(nullptr), "OGRGeoJSON");
  EXPECT_STREQ(OGRGeoJSONLayer::GetValidLayerName(""), "OGRGeoJSON");
  EXPECT_STREQ(OGRGeoJSONLayer::GetValidLayerName("custom"), "custom");

  const char kJson[] = R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "id": 1,
        "properties": {"name": "test"},
        "geometry": {"type": "Point", "coordinates": [10.0, 20.0]}
      }
    ]
  })json";

  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(kJson, GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);

  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  OGRGeoJSONLayer* poGJLayer = static_cast<OGRGeoJSONLayer*>(poLayer);
  EXPECT_EQ(poGJLayer->GetDataset(), poDS);
  EXPECT_STREQ(poGJLayer->GetFIDColumn(), "");
  poGJLayer->SetFIDColumn("custom_fid");
  EXPECT_STREQ(poGJLayer->GetFIDColumn(), "custom_fid");

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONLayerTest, InMemoryFilteringAndFeatures) {
  const char kJson[] = R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "id": 10,
        "properties": {"val": 100},
        "geometry": {"type": "Point", "coordinates": [0.0, 0.0]}
      },
      {
        "type": "Feature",
        "id": 20,
        "properties": {"val": 200},
        "geometry": {"type": "Point", "coordinates": [10.0, 10.0]}
      }
    ]
  })json";

  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(kJson, GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  EXPECT_EQ(poLayer->GetFeatureCount(), 2);
  EXPECT_EQ(poLayer->GetFeatureCount(false), 2);

  // GetFeature
  OGRFeature* poFeat10 = poLayer->GetFeature(10);
  ASSERT_NE(poFeat10, nullptr);
  EXPECT_EQ(poFeat10->GetFieldAsInteger("val"), 100);
  delete poFeat10;

  EXPECT_EQ(poLayer->GetFeature(999), nullptr);

  // Attribute filter
  poLayer->SetAttributeFilter("val = 200");
  EXPECT_EQ(poLayer->GetFeatureCount(), 1);
  poLayer->ResetReading();
  OGRFeature* poFeatNext = poLayer->GetNextFeature();
  ASSERT_NE(poFeatNext, nullptr);
  EXPECT_EQ(poFeatNext->GetFID(), 20);
  delete poFeatNext;
  EXPECT_EQ(poLayer->GetNextFeature(), nullptr);

  // Spatial filter
  poLayer->SetAttributeFilter(nullptr);
  poLayer->SetSpatialFilterRect(-1.0, -1.0, 1.0, 1.0);
  EXPECT_EQ(poLayer->GetFeatureCount(), 1);
  poLayer->ResetReading();
  poFeatNext = poLayer->GetNextFeature();
  ASSERT_NE(poFeatNext, nullptr);
  EXPECT_EQ(poFeatNext->GetFID(), 10);
  delete poFeatNext;

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONLayerTest, AddFeatureFidEdgeCases) {
  auto poDS = std::make_unique<OGRGeoJSONDataSource>();
  OGRGeoJSONLayer layer("test_layer", nullptr, wkbPoint, poDS.get(), nullptr);
  layer.SetSupportsMGeometries(true);
  layer.SetSupportsZGeometries(false);

  // 1. Add feature with explicit FID 0
  auto poFeat0 = std::make_unique<OGRFeature>(layer.GetLayerDefn());
  poFeat0->SetFID(0);
  layer.AddFeature(poFeat0.get());

  // Also pre-add feature with FID 2
  auto poFeat2 = std::make_unique<OGRFeature>(layer.GetLayerDefn());
  poFeat2->SetFID(2);
  layer.AddFeature(poFeat2.get());

  // 2. Add feature with FID -1 (auto-assign FID: GetFeatureCount is 2, which
  // collides with FID 2, exercising the auto-assign collision resolution loop).
  auto poFeatAuto = std::make_unique<OGRFeature>(layer.GetLayerDefn());
  poFeatAuto->SetFID(OGRNullFID);
  layer.AddFeature(poFeatAuto.get());
  EXPECT_EQ(poFeatAuto->GetFID(), 3);

  // 3. Pre-add FID 4
  auto poFeat4 = std::make_unique<OGRFeature>(layer.GetLayerDefn());
  poFeat4->SetFID(4);
  layer.AddFeature(poFeat4.get());

  // Add duplicate positive FID 2 (GetFeatureCount is 4, which collides with FID
  // 4, exercising the duplicate positive FID resolution loop).
  WithQuietHandler quiet;
  auto poFeatDup = std::make_unique<OGRFeature>(layer.GetLayerDefn());
  poFeatDup->SetFID(2);
  layer.AddFeature(poFeatDup.get());
  EXPECT_EQ(poFeatDup->GetFID(), 5);
  CPLErrorReset();

  // 4. Add 64-bit FID
  auto poFeat64 = std::make_unique<OGRFeature>(layer.GetLayerDefn());
  poFeat64->SetFID(1LL << 40);
  layer.AddFeature(poFeat64.get());
  EXPECT_STREQ(layer.GetMetadataItem(OLMD_FID64), "YES");
}

TEST_F(OGRGeoJSONLayerTest, DetectGeometryTypeVariants) {
  auto poDS = std::make_unique<OGRGeoJSONDataSource>();

  // 1. Known type wkbPoint returns early
  {
    OGRGeoJSONLayer layer("layer_pt", nullptr, wkbPoint, poDS.get(), nullptr);
    layer.DetectGeometryType();
    EXPECT_EQ(layer.GetGeomType(), wkbPoint);
  }

  // 2. wkbUnknown with Point features -> detects wkbPoint
  {
    OGRGeoJSONLayer layer("layer_unknown", nullptr, wkbUnknown, poDS.get(),
                          nullptr);
    auto poFeat = std::make_unique<OGRFeature>(layer.GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    poFeat->SetGeometry(&pt);
    layer.AddFeature(poFeat.get());

    layer.DetectGeometryType();
    EXPECT_EQ(layer.GetGeomType(), wkbPoint);
  }

  // 3. wkbUnknown with PointZ features -> detects wkbPoint25D
  {
    OGRGeoJSONLayer layer("layer_3d", nullptr, wkbUnknown, poDS.get(), nullptr);
    auto poFeat = std::make_unique<OGRFeature>(layer.GetLayerDefn());
    OGRPoint pt(1.0, 2.0, 3.0);
    poFeat->SetGeometry(&pt);
    layer.AddFeature(poFeat.get());

    layer.DetectGeometryType();
    EXPECT_EQ(layer.GetGeomType(), wkbPoint25D);
  }

  // 4. wkbUnknown with mixed Point and LineString features -> remains
  // wkbUnknown
  {
    OGRGeoJSONLayer layer("layer_mixed", nullptr, wkbUnknown, poDS.get(),
                          nullptr);
    auto poFeat1 = std::make_unique<OGRFeature>(layer.GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    poFeat1->SetGeometry(&pt);
    layer.AddFeature(poFeat1.get());

    auto poFeat2 = std::make_unique<OGRFeature>(layer.GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    poFeat2->SetGeometry(&ls);
    layer.AddFeature(poFeat2.get());

    layer.DetectGeometryType();
    EXPECT_EQ(layer.GetGeomType(), wkbUnknown);
  }

  // 5. wkbUnknown with null geometry features -> skipped
  {
    OGRGeoJSONLayer layer("layer_null", nullptr, wkbUnknown, poDS.get(),
                          nullptr);
    auto poFeatNull = std::make_unique<OGRFeature>(layer.GetLayerDefn());
    poFeatNull->SetGeometry(nullptr);
    layer.AddFeature(poFeatNull.get());

    layer.DetectGeometryType();
    EXPECT_EQ(layer.GetGeomType(), wkbUnknown);
  }
}

TEST_F(OGRGeoJSONLayerTest, StreamingModeOperations) {
  const std::string filename = std::string(kTestDir) + "/streaming.geojson";

  const char kJson[] = R"json({
    "type": "FeatureCollection",
    "bbox": [0.0, 0.0, 10.0, 10.0, 20.0, 30.0],
    "features": [
      {
        "type": "Feature",
        "id": 1,
        "properties": {"name": "f1"},
        "geometry": {"type": "Point", "coordinates": [1.0, 2.0, 10.0]}
      },
      {
        "type": "Feature",
        "id": 2,
        "properties": {"name": "f2"},
        "geometry": {"type": "Point", "coordinates": [3.0, 4.0, 20.0]}
      },
      {
        "type": "Feature",
        "id": 3,
        "properties": {"name": "f3"},
        "geometry": {"type": "Point", "coordinates": [5.0, 6.0, 30.0]}
      }
    ]
  })json";

  VSILFILE* fp = VSIFOpenL(filename.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kJson, 1, strlen(kJson), fp);
  VSIFCloseL(fp);

  CPLSetThreadLocalConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS", "1");
  absl::Cleanup cleanup = [] {
    CPLSetThreadLocalConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS",
                                  nullptr);
  };

  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);

  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  // TestCapability in streaming mode
  EXPECT_TRUE(poLayer->TestCapability(OLCFastGetExtent));
  EXPECT_TRUE(poLayer->TestCapability(OLCFastGetExtent3D));
  EXPECT_TRUE(poLayer->TestCapability(OLCZGeometries));
  EXPECT_TRUE(poLayer->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(poLayer->TestCapability(OLCCurveGeometries));
  EXPECT_FALSE(poLayer->TestCapability(OLCMeasuredGeometries));
  EXPECT_FALSE(poLayer->TestCapability(OLCRandomWrite));

  // IGetExtent and IGetExtent3D fast path
  OGREnvelope sExtent;
  EXPECT_EQ(poLayer->GetExtent(&sExtent, TRUE), OGRERR_NONE);
  EXPECT_EQ(poLayer->GetExtent(1, &sExtent, TRUE), OGRERR_FAILURE);

  OGREnvelope3D sExtent3D;
  EXPECT_EQ(poLayer->GetExtent3D(0, &sExtent3D, TRUE), OGRERR_NONE);
  EXPECT_EQ(poLayer->GetExtent3D(1, &sExtent3D, TRUE), OGRERR_FAILURE);

  // GetFeature in streaming mode
  OGRFeature* poFeat2 = poLayer->GetFeature(2);
  ASSERT_NE(poFeat2, nullptr);
  EXPECT_EQ(poFeat2->GetFID(), 2);
  delete poFeat2;

  // Spatial filter fallback on extent
  poLayer->SetSpatialFilterRect(0, 0, 5, 5);
  EXPECT_FALSE(poLayer->TestCapability(OLCFastGetExtent));
  EXPECT_FALSE(poLayer->TestCapability(OLCFastGetExtent3D));
  EXPECT_EQ(poLayer->GetExtent(&sExtent, TRUE), OGRERR_NONE);
  EXPECT_EQ(poLayer->GetExtent3D(0, &sExtent3D, TRUE), OGRERR_NONE);

  // Spatial filter with different SRS (creates coordinate transformation)
  OGRSpatialReference oSRS3857;
  oSRS3857.importFromEPSG(3857);
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 1000000);
  ring.addPoint(1000000, 1000000);
  ring.addPoint(1000000, 0);
  ring.addPoint(0, 0);
  poly.addRing(&ring);
  poly.assignSpatialReference(&oSRS3857);
  poLayer->SetSpatialFilter(&poly);

  poLayer->ResetReading();
  OGRFeature* poFeatFiltered = poLayer->GetNextFeature();
  ASSERT_NE(poFeatFiltered, nullptr);
  delete poFeatFiltered;

  // Clear filter and iterate all features to EOF (establishing total count)
  poLayer->SetSpatialFilter(nullptr);
  poLayer->ResetReading();
  int nCount = 0;
  while (OGRFeature* feat = poLayer->GetNextFeature()) {
    nCount++;
    delete feat;
  }
  EXPECT_EQ(nCount, 3);
  EXPECT_EQ(poLayer->GetFeatureCount(), 3);

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONLayerTest, UpdatableEasyAppendAndIngestAll) {
  const std::string filename = std::string(kTestDir) + "/append.geojson";

  // Create initial GeoJSON file ending in "}\n]\n}"
  const char kInitialJson[] =
      "{\"type\": \"FeatureCollection\", \"features\": [\n"
      "{\"type\": \"Feature\", \"properties\": {\"a\": 1}, \"geometry\": "
      "null},\n"
      "{\"type\": \"Feature\", \"properties\": {\"a\": 2}, \"geometry\": "
      "null}\n"
      "]\n}\n";

  VSILFILE* fp = VSIFOpenL(filename.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kInitialJson, 1, strlen(kInitialJson), fp);
  VSIFCloseL(fp);

  CPLSetThreadLocalConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS", "1");
  absl::Cleanup cleanup = [] {
    CPLSetThreadLocalConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS",
                                  nullptr);
  };

  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                 nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  // Read all features to EOF so nTotalFeatureCount_ >= 0 is established
  poLayer->ResetReading();
  while (OGRFeature* feat = poLayer->GetNextFeature()) {
    delete feat;
  }

  // Test capabilities in update mode
  EXPECT_TRUE(poLayer->TestCapability(OLCRandomWrite));
  EXPECT_TRUE(poLayer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(poLayer->TestCapability(OLCCreateField));
  EXPECT_TRUE(poLayer->TestCapability(OLCDeleteField));
  EXPECT_TRUE(poLayer->TestCapability(OLCReorderFields));
  EXPECT_TRUE(poLayer->TestCapability(OLCAlterFieldDefn));
  EXPECT_TRUE(poLayer->TestCapability(OLCDeleteFeature));

  // Easy append 1
  OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
  poFeat1->SetField("a", 2);
  EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
  delete poFeat1;

  // Consecutive easy append 2
  OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
  poFeat2->SetField("a", 3);
  EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
  delete poFeat2;

  // SyncToDisk terminates append session
  EXPECT_EQ(poLayer->SyncToDisk(), OGRERR_NONE);

  // Call IngestAll operations
  OGRFeature* poFeatToUpdate = poLayer->GetFeature(1);
  ASSERT_NE(poFeatToUpdate, nullptr);
  poFeatToUpdate->SetField("a", 999);
  EXPECT_EQ(poLayer->SetFeature(poFeatToUpdate), OGRERR_NONE);
  int anUpdatedFields[] = {0};
  EXPECT_EQ(poLayer->UpdateFeature(poFeatToUpdate, 1, anUpdatedFields, 0,
                                   nullptr, false),
            OGRERR_NONE);
  delete poFeatToUpdate;

  // DeleteFeature
  EXPECT_EQ(poLayer->DeleteFeature(2), OGRERR_NONE);

  // Field schema mutations (IngestAll already called)
  OGRFieldDefn oFieldDefn("new_col", OFTString);
  EXPECT_EQ(poLayer->CreateField(&oFieldDefn), OGRERR_NONE);

  OGRGeomFieldDefn oGeomFieldDefn("geom2", wkbPoint);
  EXPECT_EQ(poLayer->CreateGeomField(&oGeomFieldDefn), OGRERR_NONE);

  OGRFieldDefn oAlteredField("new_col_renamed", OFTString);
  EXPECT_EQ(poLayer->AlterFieldDefn(1, &oAlteredField, ALTER_NAME_FLAG),
            OGRERR_NONE);

  int anPanMap[] = {1, 0};
  EXPECT_EQ(poLayer->ReorderFields(anPanMap), OGRERR_NONE);
  EXPECT_EQ(poLayer->DeleteField(0), OGRERR_NONE);

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONLayerTest, ReadOnlyLayerMutationsReturnFailure) {
  const char kJson[] = R"json({
    "type": "FeatureCollection",
    "features": [
      {"type": "Feature", "properties": {"a": 1}, "geometry": null}
    ]
  })json";

  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(kJson, GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  WithQuietHandler quiet;
  OGRFeature* poFeat = poLayer->GetFeature(0);
  ASSERT_NE(poFeat, nullptr);
  EXPECT_EQ(poLayer->SetFeature(poFeat), OGRERR_FAILURE);
  int anUpdatedFields[] = {0};
  EXPECT_EQ(
      poLayer->UpdateFeature(poFeat, 1, anUpdatedFields, 0, nullptr, false),
      OGRERR_FAILURE);
  EXPECT_EQ(poLayer->DeleteFeature(0), OGRERR_FAILURE);
  EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_FAILURE);
  delete poFeat;

  OGRFieldDefn oFieldDefn("col", OFTInteger);
  EXPECT_EQ(poLayer->CreateField(&oFieldDefn), OGRERR_FAILURE);
  EXPECT_EQ(poLayer->DeleteField(0), OGRERR_FAILURE);
  int panMap[] = {0};
  EXPECT_EQ(poLayer->ReorderFields(panMap), OGRERR_FAILURE);
  EXPECT_EQ(poLayer->AlterFieldDefn(0, &oFieldDefn, ALTER_NAME_FLAG),
            OGRERR_FAILURE);
  OGRGeomFieldDefn oGeomFieldDefn("g", wkbPoint);
  EXPECT_EQ(poLayer->CreateGeomField(&oGeomFieldDefn), OGRERR_FAILURE);

  CPLErrorReset();
  GDALClose(poDS);
}

TEST_F(OGRGeoJSONLayerTest, EmptyFeatureCollectionEasyAppend) {
  const std::string filename = std::string(kTestDir) + "/empty_fc.geojson";
  const char kEmptyFC[] = "{\"type\": \"FeatureCollection\", \"features\": []}";

  VSILFILE* fp = VSIFOpenL(filename.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kEmptyFC, 1, strlen(kEmptyFC), fp);
  VSIFCloseL(fp);

  CPLSetThreadLocalConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS", "1");
  absl::Cleanup cleanup = [] {
    CPLSetThreadLocalConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS",
                                  nullptr);
  };

  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                 nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
  EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
  delete poFeat;

  GDALClose(poDS);
}

}  // namespace
}  // namespace autotest2
