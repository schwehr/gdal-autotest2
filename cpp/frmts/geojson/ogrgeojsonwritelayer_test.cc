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

// Tests for the GeoJSON write layer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsonwritelayer.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonwritelayer.cpp

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
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgeojsonwritelayer_test";

class OGRGeoJSONWriteLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRGeoJSONWriteLayerTest, BasicCapabilitiesAndAccessors) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  const std::string filename = std::string(kTestDir) + "/basic.geojson";
  GDALDataset* poDS =
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS, nullptr);

  OGRLayer* poLayer = poDS->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  ASSERT_NE(poLayer, nullptr);

  EXPECT_STREQ(poLayer->GetName(), "layer1");
  EXPECT_NE(poLayer->GetLayerDefn(), nullptr);
  EXPECT_EQ(poLayer->GetSpatialRef(), nullptr);
  EXPECT_EQ(poLayer->GetDataset(), poDS);

  // Test capabilities
  EXPECT_TRUE(poLayer->TestCapability(OLCCreateField));
  EXPECT_TRUE(poLayer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(poLayer->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(poLayer->TestCapability(OLCRandomRead));
  EXPECT_FALSE(poLayer->TestCapability("UNKNOWN"));

  // ResetReading and GetNextFeature on write layer
  poLayer->ResetReading();
  EXPECT_EQ(poLayer->GetNextFeature(), nullptr);

  // Extent before features
  OGREnvelope sExtent;
  EXPECT_EQ(poLayer->GetExtent(&sExtent, TRUE), OGRERR_FAILURE);
  EXPECT_EQ(poLayer->GetExtent(1, &sExtent, TRUE), OGRERR_FAILURE);

  // CreateField
  OGRFieldDefn oFieldDefn("str_field", OFTString);
  EXPECT_EQ(poLayer->CreateField(&oFieldDefn), OGRERR_NONE);
  // Duplicate field name returns OGRERR_NONE with debug message
  EXPECT_EQ(poLayer->CreateField(&oFieldDefn), OGRERR_NONE);

  // Write a feature
  OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
  poFeat->SetField("str_field", "val1");
  OGRPoint pt(10.0, 20.0);
  poFeat->SetGeometry(&pt);
  EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
  delete poFeat;

  // Extent after feature
  EXPECT_EQ(poLayer->GetExtent(&sExtent, TRUE), OGRERR_NONE);
  EXPECT_DOUBLE_EQ(sExtent.MinX, 10.0);
  EXPECT_DOUBLE_EQ(sExtent.MaxX, 10.0);

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONWriteLayerTest, SyncToDiskAndAppend) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  const std::string filename = std::string(kTestDir) + "/sync.geojson";
  GDALDataset* poDS =
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS, nullptr);

  OGRLayer* poLayer =
      poDS->CreateLayer("sync_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(poLayer, nullptr);

  OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
  OGRPoint pt1(1.0, 2.0);
  poFeat1->SetGeometry(&pt1);
  EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
  delete poFeat1;

  // Sync to disk closes the feature collection in the file
  EXPECT_EQ(poLayer->SyncToDisk(), OGRERR_NONE);

  // Writing another feature seeks back and replaces the closing brackets
  OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
  OGRPoint pt2(3.0, 4.0);
  poFeat2->SetGeometry(&pt2);
  EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
  delete poFeat2;

  GDALClose(poDS);

  // Reopen and verify both features are present
  GDALDataset* poDSRead = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDSRead, nullptr);
  OGRLayer* poLayerRead = poDSRead->GetLayer(0);
  ASSERT_NE(poLayerRead, nullptr);
  EXPECT_EQ(poLayerRead->GetFeatureCount(), 2);
  GDALClose(poDSRead);
}

TEST_F(OGRGeoJSONWriteLayerTest, BoundingBox2DAnd3DSeekableAndNonSeekable) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  // 1. 2D with WRITE_BBOX=YES and COORDINATE_PRECISION=3 on seekable file
  {
    const std::string filename = std::string(kTestDir) + "/bbox2d.geojson";
    const char* const apszOptions[] = {"WRITE_BBOX=YES",
                                       "COORDINATE_PRECISION=3", nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPoint, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt(1.23456, 2.34567);
    poFeat->SetGeometry(&pt);
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    delete poFeat;

    GDALClose(poDS);
  }

  // 2. 3D with WRITE_BBOX=YES and no precision (full precision)
  {
    const std::string filename = std::string(kTestDir) + "/bbox3d.geojson";
    const char* const apszOptions[] = {"WRITE_BBOX=YES", nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPoint25D, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt(1.1, 2.2, 3.3);
    poFeat->SetGeometry(&pt);
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    delete poFeat;

    GDALClose(poDS);
  }

  // 3. 3D with WRITE_BBOX=YES and explicit XY and Z precision
  {
    const std::string filename = std::string(kTestDir) + "/bbox3d_prec.geojson";
    const char* const apszOptions[] = {"WRITE_BBOX=YES", "XY_COORD_PRECISION=2",
                                       "Z_COORD_PRECISION=1", nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPoint25D, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt(1.123, 2.234, 3.345);
    poFeat->SetGeometry(&pt);
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    delete poFeat;

    GDALClose(poDS);
  }

  // 4. Non-seekable gzip file with WRITE_BBOX=YES (hits else branch in
  // FinishWriting)
  {
    const std::string filename =
        "/vsigzip/" + std::string(kTestDir) + "/bbox_gzip.geojson.gz";
    const char* const apszOptions[] = {"WRITE_BBOX=YES",
                                       "COORDINATE_PRECISION=2", nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPoint, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    poFeat->SetGeometry(&pt);
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    delete poFeat;

    GDALClose(poDS);
  }

  // 5. Non-seekable gzip file with 3D and no precision
  {
    const std::string filename =
        "/vsigzip/" + std::string(kTestDir) + "/bbox_gzip_3d.geojson.gz";
    const char* const apszOptions[] = {"WRITE_BBOX=YES", nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPoint25D, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt(10.0, 20.0, 30.0);
    poFeat->SetGeometry(&pt);
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    delete poFeat;

    GDALClose(poDS);
  }
}

TEST_F(OGRGeoJSONWriteLayerTest, CreateFeatureOptionsAndTransformations) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  const std::string filename = std::string(kTestDir) + "/options.geojson";
  const char* const apszOptions[] = {
      "FOREIGN_MEMBERS_FEATURE={\"custom_extra\":42}",
      "ID_GENERATE=YES",
      "ID_FIELD=my_id",
      "ID_TYPE=String",
      "SIGNIFICANT_FIGURES=4",
      "WRITE_NON_FINITE_VALUES=YES",
      "AUTODETECT_JSON_STRINGS=YES",
      nullptr};

  GDALDataset* poDS =
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS, nullptr);

  OGRLayer* poLayer =
      poDS->CreateLayer("layer", nullptr, wkbPoint, apszOptions);
  ASSERT_NE(poLayer, nullptr);

  OGRFieldDefn oFieldId("my_id", OFTString);
  EXPECT_EQ(poLayer->CreateField(&oFieldId), OGRERR_NONE);
  OGRFieldDefn oFieldJson("json_str", OFTString);
  EXPECT_EQ(poLayer->CreateField(&oFieldJson), OGRERR_NONE);

  OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
  poFeat->SetField("my_id", "id_100");
  poFeat->SetField("json_str", "{\"nested\": true}");
  OGRPoint pt(1.1234567, 2.2345678);
  poFeat->SetGeometry(&pt);
  EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
  delete poFeat;

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONWriteLayerTest, RFC7946ReprojectionAndValidation) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  OGRSpatialReference oSRS3857;
  oSRS3857.importFromEPSG(3857);

  // 1. Valid feature with RFC7946=YES and EPSG:3857
  {
    const std::string filename =
        std::string(kTestDir) + "/rfc7946_valid.geojson";
    const char* const apszOptions[] = {"RFC7946=YES", "WRAPDATELINE=YES",
                                       nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", &oSRS3857, wkbPoint, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    // (0, 0) in EPSG:3857 transforms to (0, 0) WGS84
    OGRPoint pt(0.0, 0.0);
    poFeat->SetGeometry(&pt);
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    delete poFeat;

    GDALClose(poDS);
  }

  // 2. GEOS precision repair path (geometry invalid after roundCoordinates)
  {
    const std::string filename =
        std::string(kTestDir) + "/rfc7946_repair.geojson";
    // COORDINATE_PRECISION=1 will round bow-tie polygon neck to pinch and
    // self-intersect
    const char* const apszOptions[] = {"RFC7946=YES", "COORDINATE_PRECISION=1",
                                       nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPolygon, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRLinearRing ring;
    ring.addPoint(0.0, 0.0);
    ring.addPoint(0.0, 1.0);
    ring.addPoint(1.0, 0.5001);
    ring.addPoint(1.0, 1.0);
    ring.addPoint(2.0, 1.0);
    ring.addPoint(2.0, 0.0);
    ring.addPoint(1.0, 0.0);
    ring.addPoint(1.0, 0.4999);
    ring.addPoint(0.0, 0.0);

    OGRPolygon poly;
    poly.addRing(&ring);
    poFeat->SetGeometry(&poly);

    WithQuietHandler quiet;
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    CPLErrorReset();

    delete poFeat;
    GDALClose(poDS);
  }

  // 3. Same GEOS precision repair path with RFC7946=NO (poFeatureToWrite ==
  // poFeature)
  {
    const std::string filename =
        std::string(kTestDir) + "/non_rfc_repair.geojson";
    const char* const apszOptions[] = {"COORDINATE_PRECISION=1", nullptr};
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbPolygon, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
    OGRLinearRing ring;
    ring.addPoint(0.0, 0.0);
    ring.addPoint(0.0, 1.0);
    ring.addPoint(1.0, 0.5001);
    ring.addPoint(1.0, 1.0);
    ring.addPoint(2.0, 1.0);
    ring.addPoint(2.0, 0.0);
    ring.addPoint(1.0, 0.0);
    ring.addPoint(1.0, 0.4999);
    ring.addPoint(0.0, 0.0);

    OGRPolygon poly;
    poly.addRing(&ring);
    poFeat->SetGeometry(&poly);

    WithQuietHandler quiet;
    EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
    CPLErrorReset();

    delete poFeat;
    GDALClose(poDS);
  }
}

TEST_F(OGRGeoJSONWriteLayerTest, AntimeridianBBoxRFC7946Merging) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  const char* const apszOptions[] = {"RFC7946=YES", "WRITE_BBOX=YES", nullptr};

  auto createCrossingAM = [](double min_e, double max_w) {
    auto poMLS = std::make_unique<OGRMultiLineString>();
    auto poLS1 = std::make_unique<OGRLineString>();
    poLS1->addPoint(min_e, 10.0);
    poLS1->addPoint(180.0, 10.0);
    auto poLS2 = std::make_unique<OGRLineString>();
    poLS2->addPoint(-180.0, 20.0);
    poLS2->addPoint(max_w, 20.0);
    poMLS->addGeometryDirectly(poLS1.release());
    poMLS->addGeometryDirectly(poLS2.release());
    return poMLS;
  };

  // Case A: Feature 1 crosses AM, Feature 2 crosses AM (both cross)
  {
    const std::string filename =
        std::string(kTestDir) + "/am_both_cross.geojson";
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbMultiLineString, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
    auto geom1 = createCrossingAM(170.0, -170.0);
    poFeat1->SetGeometry(geom1.get());
    EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
    delete poFeat1;

    OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
    auto geom2 = createCrossingAM(160.0, -160.0);
    poFeat2->SetGeometry(geom2.get());
    EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
    delete poFeat2;

    GDALClose(poDS);
  }

  // Case B: Non-crossing layer, then crossing feature
  // (MinX > 0, MaxX < 0, and spanning PM)
  {
    // B1: MinX > 0 then cross
    {
      const std::string filename =
          std::string(kTestDir) + "/am_east_then_cross.geojson";
      GDALDataset* poDS =
          poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
      ASSERT_NE(poDS, nullptr);
      OGRLayer* poLayer =
          poDS->CreateLayer("layer", nullptr, wkbUnknown, apszOptions);
      ASSERT_NE(poLayer, nullptr);

      OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
      OGRPoint pt1(175.0, 10.0);
      poFeat1->SetGeometry(&pt1);
      EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
      delete poFeat1;

      OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
      auto geom2 = createCrossingAM(170.0, -170.0);
      poFeat2->SetGeometry(geom2.get());
      EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
      delete poFeat2;

      GDALClose(poDS);
    }

    // B2: MaxX < 0 then cross
    {
      const std::string filename =
          std::string(kTestDir) + "/am_west_then_cross.geojson";
      GDALDataset* poDS =
          poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
      ASSERT_NE(poDS, nullptr);
      OGRLayer* poLayer =
          poDS->CreateLayer("layer", nullptr, wkbUnknown, apszOptions);
      ASSERT_NE(poLayer, nullptr);

      OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
      OGRPoint pt1(-175.0, 10.0);
      poFeat1->SetGeometry(&pt1);
      EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
      delete poFeat1;

      OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
      auto geom2 = createCrossingAM(170.0, -170.0);
      poFeat2->SetGeometry(geom2.get());
      EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
      delete poFeat2;

      GDALClose(poDS);
    }

    // B3: Spanning PM then cross
    {
      const std::string filename =
          std::string(kTestDir) + "/am_pm_then_cross.geojson";
      GDALDataset* poDS =
          poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
      ASSERT_NE(poDS, nullptr);
      OGRLayer* poLayer =
          poDS->CreateLayer("layer", nullptr, wkbUnknown, apszOptions);
      ASSERT_NE(poLayer, nullptr);

      OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
      OGRLineString ls1;
      ls1.addPoint(-10.0, 10.0);
      ls1.addPoint(10.0, 20.0);
      poFeat1->SetGeometry(&ls1);
      EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
      delete poFeat1;

      OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
      auto geom2 = createCrossingAM(170.0, -170.0);
      poFeat2->SetGeometry(geom2.get());
      EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
      delete poFeat2;

      GDALClose(poDS);
    }
  }

  // Case C: Crossing layer, then non-crossing features
  // (MinX > 0, MaxX < 0, and spanning PM)
  {
    const std::string filename =
        std::string(kTestDir) + "/am_cross_then_noncross.geojson";
    GDALDataset* poDS =
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer =
        poDS->CreateLayer("layer", nullptr, wkbUnknown, apszOptions);
    ASSERT_NE(poLayer, nullptr);

    // Feature 1 crossing AM
    OGRFeature* poFeat1 = new OGRFeature(poLayer->GetLayerDefn());
    auto geom1 = createCrossingAM(170.0, -170.0);
    poFeat1->SetGeometry(geom1.get());
    EXPECT_EQ(poLayer->CreateFeature(poFeat1), OGRERR_NONE);
    delete poFeat1;

    // Feature 2 Eastern (MinX > 0)
    OGRFeature* poFeat2 = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt2(165.0, 15.0);
    poFeat2->SetGeometry(&pt2);
    EXPECT_EQ(poLayer->CreateFeature(poFeat2), OGRERR_NONE);
    delete poFeat2;

    // Feature 3 Western (MaxX < 0)
    OGRFeature* poFeat3 = new OGRFeature(poLayer->GetLayerDefn());
    OGRPoint pt3(-165.0, 15.0);
    poFeat3->SetGeometry(&pt3);
    EXPECT_EQ(poLayer->CreateFeature(poFeat3), OGRERR_NONE);
    delete poFeat3;

    // Feature 4 spanning Prime Meridian [-10, 10]
    OGRFeature* poFeat4 = new OGRFeature(poLayer->GetLayerDefn());
    OGRLineString ls4;
    ls4.addPoint(-10.0, 10.0);
    ls4.addPoint(10.0, 20.0);
    poFeat4->SetGeometry(&ls4);
    EXPECT_EQ(poLayer->CreateFeature(poFeat4), OGRERR_NONE);
    delete poFeat4;

    GDALClose(poDS);
  }
}

}  // namespace
}  // namespace autotest2
