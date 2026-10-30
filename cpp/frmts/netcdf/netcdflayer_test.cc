// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cmath>
#include <cstring>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class NetCDFLayerTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFLayerTest, LegacyPointLonLatAllFieldTypes) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_point_lonlat.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  papszDSCO = CSLSetNameValue(papszDSCO, "WRITE_GDAL_TAGS", "YES");

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "RECORD_DIM_NAME", "rec_dim");
  papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "YES");
  papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "YES");

  OGRLayer* poLayer =
      poDS->CreateLayer("point_layer", &srs, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  // Add all kinds of fields
  OGRFieldDefn f_int("f_int", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_int));

  OGRFieldDefn f_int16("f_int16", OFTInteger);
  f_int16.SetSubType(OFSTInt16);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_int16));

  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_bool));

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_int64));

  OGRFieldDefn f_real("f_real", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_real));

  OGRFieldDefn f_float("f_float", OFTReal);
  f_float.SetSubType(OFSTFloat32);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_float));

  OGRFieldDefn f_str("f_str", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_str));

  OGRFieldDefn f_char1("f_char1", OFTString);
  f_char1.SetWidth(1);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_char1));

  OGRFieldDefn f_date("f_date", OFTDate);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_date));

  OGRFieldDefn f_datetime("f_datetime", OFTDateTime);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_datetime));

  // Feature 1: normal values
  {
    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("f_int", 123);
    feat->SetField("f_int16", static_cast<int>(32000));
    feat->SetField("f_bool", 1);
    feat->SetField("f_int64", static_cast<GIntBig>(9876543210LL));
    feat->SetField("f_real", 123.456);
    feat->SetField("f_float", 78.5f);
    feat->SetField("f_str", "hello netcdf");
    feat->SetField("f_char1", "A");
    feat->SetField("f_date", 2026, 10, 29, 0, 0, 0, 0);
    feat->SetField("f_datetime", 2026, 10, 29, 12, 34, 56, 0);
    feat->SetGeometryDirectly(new OGRPoint(10.5, 20.5));
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // Feature 2: unset / null values and another point
  {
    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("f_int", -456);
    feat->SetGeometryDirectly(new OGRPoint(-15.0, 45.0));
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // Feature 3: null geometry
  {
    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("f_int", 999);
    feat->SetGeometryDirectly(nullptr);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  // Test capabilities
  EXPECT_TRUE(poLayer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(poLayer->TestCapability(OLCCreateField));
  EXPECT_FALSE(poLayer->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(poLayer->TestCapability(OLCRandomRead));
  EXPECT_FALSE(poLayer->TestCapability(OLCRandomWrite));

  EXPECT_EQ(poDS.get(), poLayer->GetDataset());

  poDS.reset();

  // Re-open and verify
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    EXPECT_EQ(1, poReadDS->GetLayerCount());

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    EXPECT_STREQ("point_layer", l->GetName());
    EXPECT_EQ(3, l->GetFeatureCount(TRUE));

    // Test feature reading
    std::unique_ptr<OGRFeature> f1(l->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_EQ(123, f1->GetFieldAsInteger("f_int"));
    EXPECT_EQ(32000, f1->GetFieldAsInteger("f_int16"));
    EXPECT_EQ(1, f1->GetFieldAsInteger("f_bool"));
    EXPECT_EQ(9876543210LL, f1->GetFieldAsInteger64("f_int64"));
    EXPECT_DOUBLE_EQ(123.456, f1->GetFieldAsDouble("f_real"));
    EXPECT_FLOAT_EQ(78.5f, static_cast<float>(f1->GetFieldAsDouble("f_float")));
    EXPECT_STREQ("hello netcdf", f1->GetFieldAsString("f_str"));
    EXPECT_STREQ("A", f1->GetFieldAsString("f_char1"));

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, tz = 0;
    f1->GetFieldAsDateTime(f1->GetFieldIndex("f_date"), &year, &month, &day,
                           &hour, &minute, &second, &tz);
    EXPECT_EQ(2026, year);
    EXPECT_EQ(10, month);
    EXPECT_EQ(29, day);

    OGRGeometry* g1 = f1->GetGeometryRef();
    ASSERT_NE(nullptr, g1);
    EXPECT_EQ(wkbPoint, g1->getGeometryType());
    auto* pt1 = g1->toPoint();
    EXPECT_DOUBLE_EQ(10.5, pt1->getX());
    EXPECT_DOUBLE_EQ(20.5, pt1->getY());

    std::unique_ptr<OGRFeature> f2(l->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(-456, f2->GetFieldAsInteger("f_int"));
    OGRGeometry* g2 = f2->GetGeometryRef();
    ASSERT_NE(nullptr, g2);
    auto* pt2 = g2->toPoint();
    EXPECT_DOUBLE_EQ(-15.0, pt2->getX());
    EXPECT_DOUBLE_EQ(45.0, pt2->getY());

    std::unique_ptr<OGRFeature> f3(l->GetNextFeature());
    ASSERT_NE(nullptr, f3);
    EXPECT_EQ(999, f3->GetFieldAsInteger("f_int"));
    EXPECT_EQ(nullptr, f3->GetGeometryRef());

    // Spatial filter
    l->SetSpatialFilterRect(0, 0, 30, 30);
    l->ResetReading();
    std::unique_ptr<OGRFeature> f_filt(l->GetNextFeature());
    ASSERT_NE(nullptr, f_filt);
    EXPECT_EQ(123, f_filt->GetFieldAsInteger("f_int"));
    EXPECT_EQ(nullptr, l->GetNextFeature());

    // Attribute filter
    l->SetSpatialFilter(nullptr);
    l->SetAttributeFilter("f_int = -456");
    l->ResetReading();
    std::unique_ptr<OGRFeature> f_att(l->GetNextFeature());
    ASSERT_NE(nullptr, f_att);
    EXPECT_EQ(-456, f_att->GetFieldAsInteger("f_int"));
    EXPECT_EQ(nullptr, l->GetNextFeature());
    l->SetAttributeFilter(nullptr);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyPointProjectedAnd3D) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_point_proj3d.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(32631);  // UTM zone 31N

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  OGRLayer* poLayer =
      poDS->CreateLayer("point3d_layer", &srs, wkbPoint25D, nullptr);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_id("id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_id));

  // Add 3D Point
  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("id", 100);
  feat->SetGeometryDirectly(new OGRPoint(500000.0, 4500000.0, 150.0));
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  // Re-open and verify projected coords and Z
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(100, f->GetFieldAsInteger("id"));

    OGRGeometry* g = f->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPoint25D, g->getGeometryType());
    auto* pt = g->toPoint();
    EXPECT_DOUBLE_EQ(500000.0, pt->getX());
    EXPECT_DOUBLE_EQ(4500000.0, pt->getY());
    EXPECT_DOUBLE_EQ(150.0, pt->getZ());
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyWKTGeometries) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_wkt_geom.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
  papszLCO = CSLSetNameValue(papszLCO, "WKT_DEFAULT_WIDTH", "1000");

  OGRLayer* poLayer =
      poDS->CreateLayer("poly_wkt_layer", &srs, wkbPolygon, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_name("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_name));

  // Add polygon
  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("name", "box");
  auto* poly = new OGRPolygon();
  auto* ring = new OGRLinearRing();
  ring->addPoint(0, 0);
  ring->addPoint(10, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring);
  feat->SetGeometryDirectly(poly);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  // Re-open and verify
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_STREQ("box", f->GetFieldAsString("name"));

    OGRGeometry* g = f->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPolygon, wkbFlatten(g->getGeometryType()));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyProfileFeatureType) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_profile.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "FEATURE_TYPE", "PROFILE");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_DIM_INIT_SIZE", "10");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_DIM_NAME", "profile_dim");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_VARIABLES", "station");

  OGRLayer* poLayer =
      poDS->CreateLayer("profile_layer", &srs, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  // Field for profile id
  OGRFieldDefn f_prof("profile_dim", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_prof));

  OGRFieldDefn f_station("station", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_station));

  OGRFieldDefn f_temp("temp", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_temp));

  // Add 2 observations in profile 1
  {
    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("profile_dim", 1);
    feat->SetField("station", "Station_A");
    feat->SetField("temp", 15.2);
    feat->SetGeometryDirectly(new OGRPoint(10.0, 50.0));
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }
  {
    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("profile_dim", 1);
    feat->SetField("station", "Station_A");
    feat->SetField("temp", 14.8);
    feat->SetGeometryDirectly(new OGRPoint(10.0, 50.0));
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  poDS.reset();

  // Re-open and verify
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    EXPECT_EQ(2, l->GetFeatureCount(TRUE));
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_DOUBLE_EQ(15.2, f->GetFieldAsDouble("temp"));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, SimpleGeometryAllTypes) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "sg_all_types.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "CF_1.8");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "BUFFER_SIZE", "8192");
  papszLCO = CSLSetNameValue(papszLCO, "GROUPLESS_WRITE_BACK", "YES");

  // 1. LineString layer
  OGRLayer* poLayer = poDS->CreateLayer("lines", &srs, wkbLineString, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_id("id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_id));
  OGRFieldDefn f_val("val", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_val));
  OGRFieldDefn f_txt("txt", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_txt));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("id", 1);
  feat->SetField("val", 3.14);
  feat->SetField("txt", "line1");
  auto* ls = new OGRLineString();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  ls->addPoint(2, 0);
  feat->SetGeometryDirectly(ls);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  // Feature 2
  OGRFeature* feat2 = new OGRFeature(poLayer->GetLayerDefn());
  feat2->SetField("id", 2);
  feat2->SetField("val", 6.28);
  feat2->SetField("txt", "line2");
  auto* ls2 = new OGRLineString();
  ls2->addPoint(5, 5);
  ls2->addPoint(6, 6);
  feat2->SetGeometryDirectly(ls2);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat2));
  OGRFeature::DestroyFeature(feat2);

  poDS.reset();

  // Re-open and verify
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    EXPECT_EQ(2, l->GetFeatureCount(TRUE));

    std::unique_ptr<OGRFeature> f1(l->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_EQ(1, f1->GetFieldAsInteger("id"));
    EXPECT_DOUBLE_EQ(3.14, f1->GetFieldAsDouble("val"));
    EXPECT_STREQ("line1", f1->GetFieldAsString("txt"));
    OGRGeometry* g = f1->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbLineString, wkbFlatten(g->getGeometryType()));

    std::unique_ptr<OGRFeature> f2(l->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(2, f2->GetFieldAsInteger("id"));
    EXPECT_DOUBLE_EQ(6.28, f2->GetFieldAsDouble("val"));
    EXPECT_STREQ("line2", f2->GetFieldAsString("txt"));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, SimpleGeometryMultiPolygonWithHole3D) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "sg_mpoly_hole_3d.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "CF_1.8");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  OGRLayer* poLayer =
      poDS->CreateLayer("mpoly3d", &srs, wkbMultiPolygon25D, nullptr);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_name("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_name));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("name", "donut");

  auto* mpoly = new OGRMultiPolygon();
  auto* poly = new OGRPolygon();

  auto* ext = new OGRLinearRing();
  ext->addPoint(0, 0, 10);
  ext->addPoint(10, 0, 11);
  ext->addPoint(10, 10, 12);
  ext->addPoint(0, 10, 13);
  ext->addPoint(0, 0, 10);
  poly->addRingDirectly(ext);

  auto* in = new OGRLinearRing();
  in->addPoint(2, 2, 20);
  in->addPoint(8, 2, 21);
  in->addPoint(8, 8, 22);
  in->addPoint(2, 8, 23);
  in->addPoint(2, 2, 20);
  poly->addRingDirectly(in);

  mpoly->addGeometryDirectly(poly);
  feat->SetGeometryDirectly(mpoly);

  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  // Re-open and verify
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_STREQ("donut", f->GetFieldAsString("name"));

    OGRGeometry* g = f->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbMultiPolygon25D, g->getGeometryType());
    auto* mp = g->toMultiPolygon();
    EXPECT_EQ(1, mp->getNumGeometries());
    auto* p = mp->getGeometryRef(0);
    EXPECT_EQ(1, p->getNumInteriorRings());
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, OptionsAndErrorHandling) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  auto create_ds = [&](const std::string& fn) {
    char** papszDSCO = nullptr;
    papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
    papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
    auto ds = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
    CSLDestroy(papszDSCO);
    return ds;
  };

  // 1. FEATURE_TYPE=POINT warning on non-point layer
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "opt_err1.nc").string();
    auto ds = create_ds(fn);
    ASSERT_NE(nullptr, ds);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "FEATURE_TYPE", "POINT");
    CPLErrorReset();
    OGRLayer* l = ds->CreateLayer("bad_pt", nullptr, wkbLineString, papszLCO);
    CSLDestroy(papszLCO);
    EXPECT_NE(nullptr, l);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("FEATURE_TYPE=POINT"));

    ds.reset();
    VSIUnlink(fn.c_str());
  }

  // 2. FEATURE_TYPE=PROFILE warning on non-point layer
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "opt_err2.nc").string();
    auto ds = create_ds(fn);
    ASSERT_NE(nullptr, ds);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "FEATURE_TYPE", "PROFILE");
    CPLErrorReset();
    OGRLayer* l = ds->CreateLayer("bad_prof", nullptr, wkbPolygon, papszLCO);
    CSLDestroy(papszLCO);
    EXPECT_NE(nullptr, l);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("FEATURE_TYPE=PROFILE"));

    ds.reset();
    VSIUnlink(fn.c_str());
  }

  // 3. FEATURE_TYPE=INVALID warning
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "opt_err3.nc").string();
    auto ds = create_ds(fn);
    ASSERT_NE(nullptr, ds);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "FEATURE_TYPE", "INVALID_TYPE");
    CPLErrorReset();
    OGRLayer* l = ds->CreateLayer("bad_type", nullptr, wkbPoint, papszLCO);
    CSLDestroy(papszLCO);
    EXPECT_NE(nullptr, l);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("FEATURE_TYPE=INVALID_TYPE not supported"));

    ds.reset();
    VSIUnlink(fn.c_str());
  }

  // 4. Duplicate field name
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "opt_err4.nc").string();
    auto ds = create_ds(fn);
    ASSERT_NE(nullptr, ds);

    OGRLayer* l = ds->CreateLayer("dup_fields", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, l);
    OGRFieldDefn f1("field_a", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, l->CreateField(&f1));
    // Adding field with duplicate name should rename or handle cleanly
    OGRFieldDefn f2("field_a", OFTString);
    EXPECT_EQ(OGRERR_NONE, l->CreateField(&f2));

    ds.reset();
    VSIUnlink(fn.c_str());
  }
}

TEST_F(NetCDFLayerTest, DirectLayerMethodCalls) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "direct_layer_test.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_rec = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "rec", NC_UNLIMITED, &dim_rec));
  int dim_wkt_width = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "wkt_width", 256, &dim_wkt_width));
  int dim3 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim3", 3, &dim3));

  int v_lon = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_DOUBLE, 1, &dim_rec, &v_lon));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_lon, "standard_name",
                                      strlen("longitude"), "longitude"));
  int v_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_DOUBLE, 1, &dim_rec, &v_lat));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_lat, "standard_name",
                                      strlen("latitude"), "latitude"));

  // Define variables of different types for AddField testing
  int v_byte = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_byte", NC_BYTE, 1, &dim_rec, &v_byte));
  int v_ubyte = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_ubyte", NC_UBYTE, 1, &dim_rec, &v_ubyte));
  int v_short = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_short", NC_SHORT, 1, &dim_rec, &v_short));
  int v_ushort = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_ushort", NC_USHORT, 1, &dim_rec, &v_ushort));
  int v_int = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_int", NC_INT, 1, &dim_rec, &v_int));
  int v_uint = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_uint", NC_UINT, 1, &dim_rec, &v_uint));
  int v_int64 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_int64", NC_INT64, 1, &dim_rec, &v_int64));
  int v_uint64 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_uint64", NC_UINT64, 1, &dim_rec, &v_uint64));
  int v_float = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_float", NC_FLOAT, 1, &dim_rec, &v_float));
  int v_double = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_double", NC_DOUBLE, 1, &dim_rec, &v_double));
  int v_string = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_string", NC_STRING, 1, &dim_rec, &v_string));
  int v_date = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_date", NC_INT, 1, &dim_rec, &v_date));
  int v_datetime = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_datetime", NC_DOUBLE, 1, &dim_rec,
                                 &v_datetime));

  // Variables without _FillValue
  int v_ubyte_nf = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_ubyte_nf", NC_UBYTE, 1, &dim_rec, &v_ubyte_nf));
  int v_ushort_nf = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_ushort_nf", NC_USHORT, 1, &dim_rec,
                                 &v_ushort_nf));
  int v_uint_nf = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_uint_nf", NC_UINT, 1, &dim_rec, &v_uint_nf));
  int v_uint64_nf = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_uint64_nf", NC_UINT64, 1, &dim_rec,
                                 &v_uint64_nf));

  // Variable with ogr_field_type = "Integer64"
  int v_i64 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_i64", NC_INT, 1, &dim_rec, &v_i64));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_i64, "ogr_field_type",
                                      strlen("Integer64"), "Integer64"));

  // Variable with days units
  int v_days = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_days", NC_INT, 1, &dim_rec, &v_days));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_days, "units",
                                      strlen("days since 1970-01-01"),
                                      "days since 1970-01-01"));

  // Variable with precision
  int v_prec = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_prec", NC_DOUBLE, 1, &dim_rec, &v_prec));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_prec, "ogr_field_precision",
                                      strlen("4"), "4"));

  // 2D WKT
  int dims2d[2] = {dim_rec, dim_wkt_width};
  int v_wkt_2d = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "wkt_2d", NC_CHAR, 2, dims2d, &v_wkt_2d));

  // 3D WKT
  int dims3d[3] = {dim_rec, dim_wkt_width, dim3};
  int v_wkt_3d = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "wkt_3d", NC_CHAR, 3, dims3d, &v_wkt_3d));

  // Add _FillValue attributes
  signed char fill_byte = -127;
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(ncid, v_byte, "_FillValue", NC_BYTE, 1,
                                       &fill_byte));
  unsigned char fill_ubyte = 255;
  ASSERT_EQ(NC_NOERR, nc_put_att_uchar(ncid, v_ubyte, "_FillValue", NC_UBYTE, 1,
                                       &fill_ubyte));
  short fill_short = -32767;
  ASSERT_EQ(NC_NOERR, nc_put_att_short(ncid, v_short, "_FillValue", NC_SHORT, 1,
                                       &fill_short));
  unsigned short fill_ushort = 65535;
  ASSERT_EQ(NC_NOERR, nc_put_att_ushort(ncid, v_ushort, "_FillValue", NC_USHORT,
                                        1, &fill_ushort));
  int fill_int = -2147483647;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(ncid, v_int, "_FillValue", NC_INT, 1, &fill_int));
  unsigned int fill_uint = 4294967295U;
  ASSERT_EQ(NC_NOERR, nc_put_att_uint(ncid, v_uint, "_FillValue", NC_UINT, 1,
                                      &fill_uint));
  long long fill_int64 = -9223372036854775807LL;
  ASSERT_EQ(NC_NOERR, nc_put_att_longlong(ncid, v_int64, "_FillValue", NC_INT64,
                                          1, &fill_int64));
  unsigned long long fill_uint64 = 18446744073709551615ULL;
  ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(ncid, v_uint64, "_FillValue",
                                           NC_UINT64, 1, &fill_uint64));
  float fill_float = 1e36f;
  ASSERT_EQ(NC_NOERR, nc_put_att_float(ncid, v_float, "_FillValue", NC_FLOAT, 1,
                                       &fill_float));
  double fill_double = 1e36;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, v_double, "_FillValue", NC_DOUBLE,
                                        1, &fill_double));

  // Add date / time attributes
  const char* pszDateType = "Date";
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_date, "ogr_field_type",
                                      strlen(pszDateType), pszDateType));
  const char* pszUnitsSec = "seconds since 1970-1-1 0:0:0";
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_datetime, "units",
                                      strlen(pszUnitsSec), pszUnitsSec));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Row 0: valid data
  size_t idx0 = 0;
  signed char d_byte = 10;
  ASSERT_EQ(NC_NOERR, nc_put_var1_schar(ncid, v_byte, &idx0, &d_byte));
  unsigned char d_ubyte = 20;
  ASSERT_EQ(NC_NOERR, nc_put_var1_uchar(ncid, v_ubyte, &idx0, &d_ubyte));
  short d_short = 30;
  ASSERT_EQ(NC_NOERR, nc_put_var1_short(ncid, v_short, &idx0, &d_short));
  unsigned short d_ushort = 40;
  ASSERT_EQ(NC_NOERR, nc_put_var1_ushort(ncid, v_ushort, &idx0, &d_ushort));
  int d_int = 50;
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(ncid, v_int, &idx0, &d_int));
  unsigned int d_uint = 60;
  ASSERT_EQ(NC_NOERR, nc_put_var1_uint(ncid, v_uint, &idx0, &d_uint));
  long long d_int64 = 70;
  ASSERT_EQ(NC_NOERR, nc_put_var1_longlong(ncid, v_int64, &idx0, &d_int64));
  unsigned long long d_uint64 = 80;
  ASSERT_EQ(NC_NOERR, nc_put_var1_ulonglong(ncid, v_uint64, &idx0, &d_uint64));
  float d_float = 90.5f;
  ASSERT_EQ(NC_NOERR, nc_put_var1_float(ncid, v_float, &idx0, &d_float));
  double d_double = 100.5;
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_double, &idx0, &d_double));
  const char* d_str = "hello";
  ASSERT_EQ(NC_NOERR, nc_put_var1_string(ncid, v_string, &idx0, &d_str));
  int d_date = 86400 * 10;
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(ncid, v_date, &idx0, &d_date));
  double d_datetime = 3600.0;
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_datetime, &idx0, &d_datetime));
  double d_lon = 5.0, d_lat = 10.0;
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_lon, &idx0, &d_lon));
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_lat, &idx0, &d_lat));

  // Row 1: fill values
  size_t idx1 = 1;
  ASSERT_EQ(NC_NOERR, nc_put_var1_schar(ncid, v_byte, &idx1, &fill_byte));
  ASSERT_EQ(NC_NOERR, nc_put_var1_uchar(ncid, v_ubyte, &idx1, &fill_ubyte));
  ASSERT_EQ(NC_NOERR, nc_put_var1_short(ncid, v_short, &idx1, &fill_short));
  ASSERT_EQ(NC_NOERR, nc_put_var1_ushort(ncid, v_ushort, &idx1, &fill_ushort));
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(ncid, v_int, &idx1, &fill_int));
  ASSERT_EQ(NC_NOERR, nc_put_var1_uint(ncid, v_uint, &idx1, &fill_uint));
  ASSERT_EQ(NC_NOERR, nc_put_var1_longlong(ncid, v_int64, &idx1, &fill_int64));
  ASSERT_EQ(NC_NOERR,
            nc_put_var1_ulonglong(ncid, v_uint64, &idx1, &fill_uint64));
  ASSERT_EQ(NC_NOERR, nc_put_var1_float(ncid, v_float, &idx1, &fill_float));
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_double, &idx1, &fill_double));
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_lon, &idx1, &d_lon));
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(ncid, v_lat, &idx1, &d_lat));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  ASSERT_GE(poDS->GetLayerCount(), 1);

  OGRLayer* layer = poDS->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  auto* nclayer = dynamic_cast<netCDFLayer*>(layer);
  ASSERT_NE(nullptr, nclayer);
  EXPECT_GT(nclayer->GetCDFID(), 0);
  nclayer->SetRecordDimID(dim_rec);
  nclayer->SetCDFID(nclayer->GetCDFID());
  CPLErrorReset();
  nclayer->SetXYZVars(v_lon, v_lat, -1);
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  // Non-float/double Z triggers a warning and drops the Z variable.
  nclayer->SetXYZVars(v_lon, v_lat, v_int);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Z variable of type"));
  nclayer->SetWKTGeometryField("wkt_var");  // Non-existent variable.
  nclayer->SetWKTGeometryField("wkt_2d");   // Valid (record, width) char var.
  nclayer->SetWKTGeometryField("wkt_3d");   // 3D char var is rejected.
  nclayer->SetGridMapping("grid_map");
  nclayer->SetProfile(-1, -1);
  EXPECT_FALSE(nclayer->AddField(-999));
  EXPECT_GE(nclayer->GetLayerDefn()->GetFieldCount(), 1);

  layer->ResetReading();
  std::unique_ptr<OGRFeature> f1(layer->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_EQ(10, f1->GetFieldAsInteger("v_byte"));
  EXPECT_EQ(20, f1->GetFieldAsInteger("v_ubyte"));
  EXPECT_EQ(30, f1->GetFieldAsInteger("v_short"));
  EXPECT_EQ(40, f1->GetFieldAsInteger("v_ushort"));
  EXPECT_EQ(50, f1->GetFieldAsInteger("v_int"));
  EXPECT_EQ(60, f1->GetFieldAsInteger("v_uint"));
  EXPECT_EQ(70, f1->GetFieldAsInteger64("v_int64"));
  EXPECT_DOUBLE_EQ(80.0, f1->GetFieldAsDouble("v_uint64"));
  EXPECT_FLOAT_EQ(90.5f, static_cast<float>(f1->GetFieldAsDouble("v_float")));
  EXPECT_DOUBLE_EQ(100.5, f1->GetFieldAsDouble("v_double"));
  EXPECT_STREQ("hello", f1->GetFieldAsString("v_string"));
  EXPECT_TRUE(f1->IsFieldSet(f1->GetFieldIndex("v_date")));
  EXPECT_TRUE(f1->IsFieldSet(f1->GetFieldIndex("v_datetime")));
  // The rejected 3D WKT variable leaves the layer reading geometry from the
  // lon/lat variables, and the integer Z variable was dropped.
  {
    OGRGeometry* g = f1->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPoint, g->getGeometryType());
    EXPECT_DOUBLE_EQ(5.0, g->toPoint()->getX());
    EXPECT_DOUBLE_EQ(10.0, g->toPoint()->getY());
  }

  // Switching the layer out of legacy mode makes the reader skip the
  // lon/lat geometry assembly: fields are still read, geometry is not.
  nclayer->EnableSGBypass();
  std::unique_ptr<OGRFeature> f2(layer->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_FALSE(f2->IsFieldSetAndNotNull(f2->GetFieldIndex("v_byte")));
  EXPECT_FALSE(f2->IsFieldSetAndNotNull(f2->GetFieldIndex("v_int")));
  EXPECT_FALSE(f2->IsFieldSetAndNotNull(f2->GetFieldIndex("v_double")));
  EXPECT_EQ(nullptr, f2->GetGeometryRef());
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  poDS.reset();
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, WriterConfigIntegration) {
  const std::string xml_config =
      "<netCDFWriterConfiguration>\n"
      "  <attribute name=\"Conventions\" value=\"CF-1.6\"/>\n"
      "  <attribute name=\"global_int\" type=\"integer\" value=\"42\"/>\n"
      "  <attribute name=\"global_double\" type=\"double\" value=\"3.14\"/>\n"
      "  <attribute name=\"to_del\" value=\"\"/>\n"
      "  <Field name=\"global_field\">\n"
      "    <attribute name=\"g_att\" value=\"g_val\"/>\n"
      "  </Field>\n"
      "  <Layer name=\"conf_layer\" netcdf_name=\"nc_conf_layer\">\n"
      "    <attribute name=\"layer_att\" value=\"layer_val\"/>\n"
      "    <Field name=\"conf_field\" netcdf_name=\"nc_conf_field\">\n"
      "      <attribute name=\"field_att\" value=\"field_val\"/>\n"
      "    </Field>\n"
      "    <Field netcdf_name=\"lon\">\n"
      "      <attribute name=\"auto_att\" value=\"auto_val\"/>\n"
      "    </Field>\n"
      "  </Layer>\n"
      "</netCDFWriterConfiguration>\n";

  const std::string config_path = "/vsimem/layer_test_config.xml";
  VSIFCloseL(VSIFileFromMemBuffer(
      config_path.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(xml_config.data())),
      xml_config.size(), FALSE));

  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "writer_config_test.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "CONFIG_FILE", config_path.c_str());
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  OGRLayer* poLayer =
      poDS->CreateLayer("conf_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_conf("conf_field", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_conf));
  OGRFieldDefn f_glob("global_field", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_glob));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("conf_field", 99);
  feat->SetField("global_field", "gf");
  feat->SetGeometryDirectly(new OGRPoint(1, 2));
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  // Re-open with GDAL: the feature must round-trip.
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    ASSERT_EQ(1, poReadDS->GetLayerCount());
    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    EXPECT_GE(l->GetLayerDefn()->GetFieldIndex("conf_field"), 0);
    EXPECT_GE(l->GetLayerDefn()->GetFieldIndex("global_field"), 0);
    std::unique_ptr<OGRFeature> rf(l->GetNextFeature());
    ASSERT_NE(nullptr, rf);
    EXPECT_EQ(99, rf->GetFieldAsInteger("conf_field"));
    EXPECT_STREQ("gf", rf->GetFieldAsString("global_field"));
    OGRGeometry* g = rf->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPoint, g->getGeometryType());
    EXPECT_DOUBLE_EQ(1.0, g->toPoint()->getX());
    EXPECT_DOUBLE_EQ(2.0, g->toPoint()->getY());
  }

  // Re-open with the netCDF API: global and layer attributes from the config
  // landed in the root group (single-layer mode), the field was renamed via
  // netcdf_name and carries the per-field attributes from the config.
  {
    int ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_open(filename.c_str(), NC_NOWRITE, &ncid));
    char att[64] = {};
    size_t len = 0;

    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, NC_GLOBAL, "Conventions", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, NC_GLOBAL, "Conventions", att));
    EXPECT_STREQ("CF-1.6", att);

    int global_int = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(ncid, NC_GLOBAL, "global_int", &global_int));
    EXPECT_EQ(42, global_int);
    double global_double = 0.0;
    ASSERT_EQ(NC_NOERR, nc_get_att_double(ncid, NC_GLOBAL, "global_double",
                                          &global_double));
    EXPECT_DOUBLE_EQ(3.14, global_double);
    // An empty value in the config deletes the attribute.
    EXPECT_EQ(NC_ENOTATT, nc_inq_attlen(ncid, NC_GLOBAL, "to_del", &len));

    memset(att, 0, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, NC_GLOBAL, "layer_att", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, NC_GLOBAL, "layer_att", att));
    EXPECT_STREQ("layer_val", att);

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "nc_conf_field", &varid));
    memset(att, 0, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, varid, "field_att", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, varid, "field_att", att));
    EXPECT_STREQ("field_val", att);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "global_field", &varid));
    memset(att, 0, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, varid, "g_att", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, varid, "g_att", att));
    EXPECT_STREQ("g_val", att);

    // "lon" is an automatically created coordinate variable, matched through
    // the config's netcdf_name.
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "lon", &varid));
    memset(att, 0, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, varid, "auto_att", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, varid, "auto_att", att));
    EXPECT_STREQ("auto_val", att);
    ASSERT_EQ(NC_NOERR, nc_close(ncid));
  }

  VSIUnlink(config_path.c_str());
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, SimpleGeometryAllFieldTypesAndAlternativeNames) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "sg_all_fields.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "CF_1.8");
  papszDSCO = CSLSetNameValue(papszDSCO, "WRITE_GDAL_TAGS", "YES");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "YES");
  OGRLayer* poLayer =
      poDS->CreateLayer("sg_fields_layer", &srs, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_int("f_int", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_int));

  OGRFieldDefn f_short("f_short", OFTInteger);
  f_short.SetSubType(OFSTInt16);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_short));

  OGRFieldDefn f_byte("f_byte", OFTInteger);
  f_byte.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_byte));

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_int64));

  OGRFieldDefn f_float("f_float", OFTReal);
  f_float.SetSubType(OFSTFloat32);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_float));

  OGRFieldDefn f_double("f_double", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_double));

  OGRFieldDefn f_str("f_str", OFTString);
  f_str.SetAlternativeName("standard_var_name");
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_str));

  OGRFieldDefn f_alt_nonstd("f_nonstd", OFTString);
  f_alt_nonstd.SetAlternativeName("NonStandard Name");
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_alt_nonstd));

  OGRFieldDefn f_comment("f_comment", OFTString);
  f_comment.SetComment("This is a detailed comment");
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_comment));

  OGRFieldDefn f_date("f_date", OFTDate);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_date));

  OGRFieldDefn f_datetime("f_datetime", OFTDateTime);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_datetime));

  OGRFieldDefn f_char1("f_char1", OFTString);
  f_char1.SetWidth(1);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_char1));

  OGRFieldDefn f_str_def("f_str_def", OFTString);
  f_str_def.SetWidth(0);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_str_def));

  OGRFieldDefn f_time("f_time", OFTTime);
  EXPECT_EQ(OGRERR_FAILURE, poLayer->CreateField(&f_time));

  // Add feature with values
  {
    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("f_int", 12345);
    feat->SetField("f_short", static_cast<int>(1234));
    feat->SetField("f_byte", 1);
    feat->SetField("f_int64", static_cast<GIntBig>(5000000000LL));
    feat->SetField("f_float", 12.5f);
    feat->SetField("f_double", 98.7654);
    feat->SetField("f_str", "a very long string that will grow the dimension");
    feat->SetField("f_nonstd", "val1");
    feat->SetField("f_comment", "val2");
    feat->SetField("f_date", 2026, 10, 29, 0, 0, 0, 0);
    feat->SetField("f_datetime", 2026, 10, 29, 14, 30, 0, 0);
    feat->SetField("f_char1", "X");
    feat->SetField("f_str_def", "default_width");
    feat->SetGeometryDirectly(new OGRPoint(10.0, 20.0));
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  }

  poDS.reset();

  // Re-open and verify
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(12345, f->GetFieldAsInteger("f_int"));
    EXPECT_EQ(1234, f->GetFieldAsInteger("f_short"));
    EXPECT_EQ(1, f->GetFieldAsInteger("f_byte"));
    EXPECT_EQ(5000000000LL, f->GetFieldAsInteger64("f_int64"));
    EXPECT_FLOAT_EQ(12.5f, static_cast<float>(f->GetFieldAsDouble("f_float")));
    EXPECT_DOUBLE_EQ(98.7654, f->GetFieldAsDouble("f_double"));
    EXPECT_STREQ("a very long string that will grow the dimension",
                 f->GetFieldAsString("f_str"));
    EXPECT_STREQ("X", f->GetFieldAsString("f_char1"));
    EXPECT_STREQ("default_width", f->GetFieldAsString("f_str_def"));

    OGRGeometry* g = f->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPoint, wkbFlatten(g->getGeometryType()));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, PointFloatCoordinates) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "point_float_coords.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_rec = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "records", NC_UNLIMITED, &dim_rec));

  int v_lon = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_FLOAT, 1, &dim_rec, &v_lon));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_lon, "standard_name",
                                      strlen("longitude"), "longitude"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_lon, "units",
                                      strlen("degrees_east"), "degrees_east"));
  float fill_lon = 1e36f;
  ASSERT_EQ(NC_NOERR, nc_put_att_float(ncid, v_lon, "_FillValue", NC_FLOAT, 1,
                                       &fill_lon));

  int v_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_FLOAT, 1, &dim_rec, &v_lat));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_lat, "standard_name",
                                      strlen("latitude"), "latitude"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_lat, "units", strlen("degrees_north"),
                            "degrees_north"));
  ASSERT_EQ(NC_NOERR, nc_put_att_float(ncid, v_lat, "_FillValue", NC_FLOAT, 1,
                                       &fill_lon));

  int v_z = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "z", NC_FLOAT, 1, &dim_rec, &v_z));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_z, "standard_name",
                                      strlen("height"), "height"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_z, "axis", strlen("Z"), "Z"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_float(ncid, v_z, "_FillValue", NC_FLOAT, 1, &fill_lon));

  int v_val = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_INT, 1, &dim_rec, &v_val));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  size_t start0 = 0, count2 = 2;
  float lons[2] = {10.5f, -5.25f};
  float lats[2] = {20.5f, 15.75f};
  float zs[2] = {100.0f, 200.0f};
  int vals[2] = {100, 200};

  ASSERT_EQ(NC_NOERR, nc_put_vara_float(ncid, v_lon, &start0, &count2, lons));
  ASSERT_EQ(NC_NOERR, nc_put_vara_float(ncid, v_lat, &start0, &count2, lats));
  ASSERT_EQ(NC_NOERR, nc_put_vara_float(ncid, v_z, &start0, &count2, zs));
  ASSERT_EQ(NC_NOERR, nc_put_vara_int(ncid, v_val, &start0, &count2, vals));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  {
    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_GE(poDS->GetLayerCount(), 1);

    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    EXPECT_EQ(2, l->GetFeatureCount(TRUE));

    std::unique_ptr<OGRFeature> f1(l->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_EQ(100, f1->GetFieldAsInteger("data"));
    OGRGeometry* g1 = f1->GetGeometryRef();
    ASSERT_NE(nullptr, g1);
    auto* pt1 = g1->toPoint();
    EXPECT_FLOAT_EQ(10.5f, static_cast<float>(pt1->getX()));
    EXPECT_FLOAT_EQ(20.5f, static_cast<float>(pt1->getY()));
    EXPECT_FLOAT_EQ(100.0f, static_cast<float>(pt1->getZ()));

    // Create a 3rd feature to trigger writing float coords
    OGRFeature* f3 = new OGRFeature(l->GetLayerDefn());
    f3->SetField("data", 300);
    f3->SetGeometryDirectly(new OGRPoint(1.5, 2.5, 3.5));
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f3));
    OGRFeature::DestroyFeature(f3);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyWKTStringInNC4) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_wkt_string.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "YES");

  OGRLayer* poLayer =
      poDS->CreateLayer("lines_wkt", nullptr, wkbLineString, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_name("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_name));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("name", "string_line");
  auto* ls = new OGRLineString();
  ls->addPoint(1, 2);
  ls->addPoint(3, 4);
  feat->SetGeometryDirectly(ls);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_STREQ("string_line", f->GetFieldAsString("name"));
    OGRGeometry* g = f->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbLineString, wkbFlatten(g->getGeometryType()));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyProfile3DAndZ) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_profile_3d.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "FEATURE_TYPE", "PROFILE");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_DIM_NAME", "p_dim");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_VARIABLES", "station");

  OGRLayer* poLayer =
      poDS->CreateLayer("prof_3d_layer", nullptr, wkbPoint25D, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_pdim("p_dim", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_pdim));
  OGRFieldDefn f_station("station", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_station));
  OGRFieldDefn f_temp("temp", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_temp));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("p_dim", 1);
  feat->SetField("station", "Station_1");
  feat->SetField("temp", 21.5);
  feat->SetGeometryDirectly(new OGRPoint(10.0, 20.0, 30.0));
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    OGRGeometry* g = f->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPoint25D, g->getGeometryType());
    auto* pt = g->toPoint();
    EXPECT_DOUBLE_EQ(10.0, pt->getX());
    EXPECT_DOUBLE_EQ(20.0, pt->getY());
    EXPECT_DOUBLE_EQ(30.0, pt->getZ());
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, WKTAutoGrowAndTruncation) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. AutoGrow WKT
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "wkt_autogrow.nc")
            .string();
    char** papszDSCO = nullptr;
    papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
    papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
    CSLDestroy(papszDSCO);
    ASSERT_NE(nullptr, poDS);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "WKT_DEFAULT_WIDTH", "20");
    papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "YES");
    papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
    OGRLayer* l =
        poDS->CreateLayer("grow_layer", nullptr, wkbLineString, papszLCO);
    CSLDestroy(papszLCO);
    ASSERT_NE(nullptr, l);

    OGRFeature* f = new OGRFeature(l->GetLayerDefn());
    auto* ls = new OGRLineString();
    ls->addPoint(0, 0);
    ls->addPoint(100, 100);
    ls->addPoint(200, 200);
    f->SetGeometryDirectly(ls);
    CPLErrorReset();
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    OGRFeature::DestroyFeature(f);

    poDS.reset();

    // The WKT (33 chars) exceeded the initial width of 20, so the width
    // dimension must have grown and the geometry must round-trip intact.
    {
      std::unique_ptr<GDALDataset> poReadDS(
          static_cast<GDALDataset*>(GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR,
                                               nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poReadDS);
      std::unique_ptr<OGRFeature> rf(poReadDS->GetLayer(0)->GetNextFeature());
      ASSERT_NE(nullptr, rf);
      OGRGeometry* g = rf->GetGeometryRef();
      ASSERT_NE(nullptr, g);
      char* wkt = nullptr;
      EXPECT_EQ(OGRERR_NONE, g->exportToWkt(&wkt));
      EXPECT_STREQ("LINESTRING (0 0,100 100,200 200)", wkt);
      CPLFree(wkt);
    }
    VSIUnlink(filename.c_str());
  }

  // 2. Truncation / failure when AutoGrow is NO
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "wkt_no_grow.nc")
            .string();
    char** papszDSCO = nullptr;
    papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
    papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
    CSLDestroy(papszDSCO);
    ASSERT_NE(nullptr, poDS);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "WKT_DEFAULT_WIDTH", "10");
    papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "NO");
    papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
    OGRLayer* l =
        poDS->CreateLayer("no_grow_layer", nullptr, wkbLineString, papszLCO);
    CSLDestroy(papszLCO);
    ASSERT_NE(nullptr, l);

    OGRFeature* f = new OGRFeature(l->GetLayerDefn());
    auto* ls = new OGRLineString();
    ls->addPoint(0, 0);
    ls->addPoint(100, 100);
    f->SetGeometryDirectly(ls);
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    // The driver reports a failure but still returns OGRERR_NONE: the WKT is
    // simply not written.
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f));
    CPLPopErrorHandler();
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot write geometry as WKT"));
    OGRFeature::DestroyFeature(f);

    poDS.reset();

    // Nothing was written for the record, so the layer reads back empty.
    {
      std::unique_ptr<GDALDataset> poReadDS(
          static_cast<GDALDataset*>(GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR,
                                               nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poReadDS);
      OGRLayer* rl = poReadDS->GetLayer(0);
      ASSERT_NE(nullptr, rl);
      EXPECT_EQ(0, rl->GetFeatureCount());
      EXPECT_EQ(nullptr, rl->GetNextFeature());
    }
    VSIUnlink(filename.c_str());
  }

  // 3. Null geometry with USE_STRING_IN_NC4 and NCDUMP_COMPAT
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "wkt_null_str.nc")
            .string();
    char** papszDSCO = nullptr;
    papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
    papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
    CSLDestroy(papszDSCO);
    ASSERT_NE(nullptr, poDS);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "YES");
    papszLCO = CSLSetNameValue(papszLCO, "NCDUMP_COMPAT", "YES");
    OGRLayer* l =
        poDS->CreateLayer("null_str_layer", nullptr, wkbLineString, papszLCO);
    CSLDestroy(papszLCO);
    ASSERT_NE(nullptr, l);

    OGRFeature* f = new OGRFeature(l->GetLayerDefn());
    f->SetGeometryDirectly(nullptr);
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f));
    OGRFeature::DestroyFeature(f);

    OGRFeature* f2 = new OGRFeature(l->GetLayerDefn());
    auto* ls = new OGRLineString();
    ls->addPoint(1, 1);
    ls->addPoint(2, 2);
    f2->SetGeometryDirectly(ls);
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f2));
    OGRFeature::DestroyFeature(f2);

    poDS.reset();

    // The null geometry is stored as an empty string placeholder (NCDUMP
    // compat), so both records are present in order.
    {
      std::unique_ptr<GDALDataset> poReadDS(
          static_cast<GDALDataset*>(GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR,
                                               nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poReadDS);
      OGRLayer* rl = poReadDS->GetLayer(0);
      ASSERT_NE(nullptr, rl);
      EXPECT_EQ(2, rl->GetFeatureCount());
      std::unique_ptr<OGRFeature> rf1(rl->GetNextFeature());
      ASSERT_NE(nullptr, rf1);
      EXPECT_EQ(nullptr, rf1->GetGeometryRef());
      std::unique_ptr<OGRFeature> rf2(rl->GetNextFeature());
      ASSERT_NE(nullptr, rf2);
      OGRGeometry* g = rf2->GetGeometryRef();
      ASSERT_NE(nullptr, g);
      char* wkt = nullptr;
      EXPECT_EQ(OGRERR_NONE, g->exportToWkt(&wkt));
      EXPECT_STREQ("LINESTRING (1 1,2 2)", wkt);
      CPLFree(wkt);
    }
    VSIUnlink(filename.c_str());
  }
}

TEST_F(NetCDFLayerTest, WriterConfigMainDim) {
  const std::string xml_config =
      "<netCDFWriterConfiguration>\n"
      "  <Layer name=\"dim_layer\">\n"
      "    <Field name=\"valid_dim_field\" main_dim=\"rec_dim\"/>\n"
      "    <Field name=\"missing_dim_field\" main_dim=\"non_existent_dim\"/>\n"
      "  </Layer>\n"
      "</netCDFWriterConfiguration>\n";

  const std::string config_path = "/vsimem/main_dim_cfg.xml";
  VSIFCloseL(VSIFileFromMemBuffer(
      config_path.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(xml_config.data())),
      xml_config.size(), FALSE));

  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "main_dim_test.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "CONFIG_FILE", config_path.c_str());
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "RECORD_DIM_NAME", "rec_dim");
  OGRLayer* l = poDS->CreateLayer("dim_layer", nullptr, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, l);

  OGRFieldDefn f1("valid_dim_field", OFTInteger);
  CPLErrorReset();
  EXPECT_EQ(OGRERR_NONE, l->CreateField(&f1));
  EXPECT_EQ(CE_None, CPLGetLastErrorType());

  // A main_dim that does not exist is reported, but the field is still
  // created against the record dimension.
  OGRFieldDefn f2("missing_dim_field", OFTInteger);
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(OGRERR_NONE, l->CreateField(&f2));
  CPLPopErrorHandler();
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Dimension 'non_existent_dim' does not exist"));

  OGRFeature* feat = new OGRFeature(l->GetLayerDefn());
  feat->SetField("valid_dim_field", 7);
  feat->SetField("missing_dim_field", 8);
  feat->SetGeometryDirectly(new OGRPoint(1, 2));
  EXPECT_EQ(OGRERR_NONE, l->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    OGRLayer* rl = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, rl);
    EXPECT_EQ(2, rl->GetLayerDefn()->GetFieldCount());
    std::unique_ptr<OGRFeature> rf(rl->GetNextFeature());
    ASSERT_NE(nullptr, rf);
    EXPECT_EQ(7, rf->GetFieldAsInteger("valid_dim_field"));
    EXPECT_EQ(8, rf->GetFieldAsInteger("missing_dim_field"));
  }

  // Both variables are indexed by the record dimension "rec_dim".
  {
    int ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_open(filename.c_str(), NC_NOWRITE, &ncid));
    int rec_dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "rec_dim", &rec_dimid));
    for (const char* name : {"valid_dim_field", "missing_dim_field"}) {
      int varid = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, name, &varid)) << name;
      int ndims = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_varndims(ncid, varid, &ndims));
      ASSERT_EQ(1, ndims) << name;
      int dimid = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, varid, &dimid));
      EXPECT_EQ(rec_dimid, dimid) << name;
    }
    ASSERT_EQ(NC_NOERR, nc_close(ncid));
  }

  VSIUnlink(config_path.c_str());
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, CF18CharFieldsAndGrowing) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "cf18_char_fields.nc")
          .string();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "CF_1.8");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
  papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "YES");
  OGRLayer* poLayer = poDS->CreateLayer("char_layer", &srs, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_char1("f_char1", OFTString);
  f_char1.SetWidth(1);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_char1));

  OGRFieldDefn f_grow("f_grow", OFTString);
  f_grow.SetWidth(4);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_grow));

  OGRFieldDefn f_fixed("f_fixed", OFTString);
  f_fixed.SetWidth(4);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_fixed));

  // Feature 1: triggers 1-char truncation warning and dim growth
  {
    OGRFeature* f = new OGRFeature(poLayer->GetLayerDefn());
    f->SetField("f_char1", "AB");  // > 1 char triggers warning
    f->SetField("f_grow",
                "much longer than 4 chars");  // triggers virtual dim resize
    f->SetField("f_fixed", "ok");
    f->SetGeometryDirectly(new OGRPoint(1, 2));
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(f));
    OGRFeature::DestroyFeature(f);
  }

  poDS.reset();

  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    OGRLayer* l = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_STREQ("A", f->GetFieldAsString("f_char1"));
    EXPECT_STREQ("much longer than 4 chars", f->GetFieldAsString("f_grow"));
    EXPECT_STREQ("ok", f->GetFieldAsString("f_fixed"));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyProfileAutoProfileId) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "legacy_profile_auto.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "FEATURE_TYPE", "PROFILE");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_DIM_NAME", "p_dim");
  papszLCO = CSLSetNameValue(papszLCO, "PROFILE_VARIABLES", "station");
  OGRLayer* poLayer =
      poDS->CreateLayer("prof_auto_layer", nullptr, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_station("station", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_station));
  OGRFieldDefn f_val("val", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_val));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("station", "Station_1");
  feat->SetField("val", 12.3);
  feat->SetGeometryDirectly(new OGRPoint(1, 2));
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  // Re-open with GDAL: the profile is reassembled into one feature.
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    OGRLayer* rl = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, rl);
    EXPECT_EQ(1, rl->GetFeatureCount());
    std::unique_ptr<OGRFeature> rf(rl->GetNextFeature());
    ASSERT_NE(nullptr, rf);
    EXPECT_STREQ("Station_1", rf->GetFieldAsString("station"));
    EXPECT_DOUBLE_EQ(12.3, rf->GetFieldAsDouble("val"));
    OGRGeometry* g = rf->GetGeometryRef();
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(wkbPoint, g->getGeometryType());
    EXPECT_DOUBLE_EQ(1.0, g->toPoint()->getX());
    EXPECT_DOUBLE_EQ(2.0, g->toPoint()->getY());
  }

  // Re-open with the netCDF API: the auto profile id variable and the
  // profile dimension were created, and the PROFILE_VARIABLES field is
  // indexed by the profile dimension.
  {
    int ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_open(filename.c_str(), NC_NOWRITE, &ncid));
    int p_dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "p_dim", &p_dimid));

    int p_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "p_dim", &p_varid));
    char att[64] = {};
    size_t len = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, p_varid, "cf_role", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, p_varid, "cf_role", att));
    EXPECT_STREQ("profile_id", att);

    int station_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "station", &station_varid));
    int ndims = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(ncid, station_varid, &ndims));
    ASSERT_EQ(1, ndims);
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, station_varid, &dimid));
    EXPECT_EQ(p_dimid, dimid);

    memset(att, 0, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ncid, NC_GLOBAL, "featureType", &len));
    ASSERT_LT(len, sizeof(att));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, NC_GLOBAL, "featureType", att));
    EXPECT_STREQ("profile", att);
    ASSERT_EQ(NC_NOERR, nc_close(ncid));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerTest, LegacyStringGrowthAndTruncation) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // Growth
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "leg_str_grow.nc")
            .string();
    char** papszDSCO = nullptr;
    papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
    papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
    CSLDestroy(papszDSCO);
    ASSERT_NE(nullptr, poDS);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
    papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "YES");
    papszLCO = CSLSetNameValue(papszLCO, "STRING_DEFAULT_WIDTH", "4");
    OGRLayer* l = poDS->CreateLayer("grow", nullptr, wkbPoint, papszLCO);
    CSLDestroy(papszLCO);
    ASSERT_NE(nullptr, l);

    OGRFieldDefn f_str("f_str", OFTString);
    EXPECT_EQ(OGRERR_NONE, l->CreateField(&f_str));

    OGRFeature* f = new OGRFeature(l->GetLayerDefn());
    f->SetField("f_str", "longer than 4");
    f->SetGeometryDirectly(new OGRPoint(1, 2));
    CPLErrorReset();
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    OGRFeature::DestroyFeature(f);

    poDS.reset();

    // The max-width dimension grew, so the full string is preserved.
    {
      std::unique_ptr<GDALDataset> poReadDS(
          static_cast<GDALDataset*>(GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR,
                                               nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poReadDS);
      std::unique_ptr<OGRFeature> rf(poReadDS->GetLayer(0)->GetNextFeature());
      ASSERT_NE(nullptr, rf);
      EXPECT_STREQ("longer than 4", rf->GetFieldAsString("f_str"));

      int ncid = -1;
      ASSERT_EQ(NC_NOERR, nc_open(filename.c_str(), NC_NOWRITE, &ncid));
      int dimid = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "f_str_max_width", &dimid));
      size_t dimlen = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ncid, dimid, &dimlen));
      EXPECT_GE(dimlen, strlen("longer than 4"));
      ASSERT_EQ(NC_NOERR, nc_close(ncid));
    }
    VSIUnlink(filename.c_str());
  }

  // Truncation
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "leg_str_trunc.nc")
            .string();
    char** papszDSCO = nullptr;
    papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
    papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
    CSLDestroy(papszDSCO);
    ASSERT_NE(nullptr, poDS);

    char** papszLCO = nullptr;
    papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
    papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "NO");
    OGRLayer* l = poDS->CreateLayer("trunc", nullptr, wkbPoint, papszLCO);
    CSLDestroy(papszLCO);
    ASSERT_NE(nullptr, l);

    OGRFieldDefn f_str("f_str", OFTString);
    f_str.SetWidth(4);
    EXPECT_EQ(OGRERR_NONE, l->CreateField(&f_str));

    OGRFeature* f = new OGRFeature(l->GetLayerDefn());
    f->SetField("f_str", "toolong");
    f->SetGeometryDirectly(new OGRPoint(1, 2));
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f));
    CPLPopErrorHandler();
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("exceeded the 4 character limit"));
    OGRFeature::DestroyFeature(f);

    // The truncation warning is only emitted once per field.
    OGRFeature* f2 = new OGRFeature(l->GetLayerDefn());
    f2->SetField("f_str", "another long one");
    f2->SetGeometryDirectly(new OGRPoint(3, 4));
    CPLErrorReset();
    EXPECT_EQ(OGRERR_NONE, l->CreateFeature(f2));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    OGRFeature::DestroyFeature(f2);

    poDS.reset();

    {
      std::unique_ptr<GDALDataset> poReadDS(
          static_cast<GDALDataset*>(GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR,
                                               nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poReadDS);
      OGRLayer* rl = poReadDS->GetLayer(0);
      ASSERT_NE(nullptr, rl);
      EXPECT_EQ(4,
                rl->GetLayerDefn()
                    ->GetFieldDefn(rl->GetLayerDefn()->GetFieldIndex("f_str"))
                    ->GetWidth());
      std::unique_ptr<OGRFeature> rf(rl->GetNextFeature());
      ASSERT_NE(nullptr, rf);
      EXPECT_STREQ("tool", rf->GetFieldAsString("f_str"));
      std::unique_ptr<OGRFeature> rf2(rl->GetNextFeature());
      ASSERT_NE(nullptr, rf2);
      EXPECT_STREQ("anot", rf2->GetFieldAsString("f_str"));
    }
    VSIUnlink(filename.c_str());
  }
}

TEST_F(NetCDFLayerTest, CF18DefaultMaxWidthDim) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "cf18_def_dim.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszDSCO = nullptr;
  papszDSCO = CSLSetNameValue(papszDSCO, "FORMAT", "NC4");
  papszDSCO = CSLSetNameValue(papszDSCO, "GEOMETRY_ENCODING", "CF_1.8");
  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszDSCO));
  CSLDestroy(papszDSCO);
  ASSERT_NE(nullptr, poDS);

  char** papszLCO = nullptr;
  papszLCO = CSLSetNameValue(papszLCO, "USE_STRING_IN_NC4", "NO");
  papszLCO = CSLSetNameValue(papszLCO, "AUTOGROW_STRINGS", "NO");
  OGRLayer* poLayer =
      poDS->CreateLayer("def_dim_layer", nullptr, wkbPoint, papszLCO);
  CSLDestroy(papszLCO);
  ASSERT_NE(nullptr, poLayer);

  OGRFieldDefn f_str0("f_str0", OFTString);
  f_str0.SetWidth(0);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_str0));

  OGRFieldDefn f_str1("f_str1", OFTString);
  f_str1.SetWidth(0);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&f_str1));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("f_str0", "zero");
  feat->SetField("f_str1", "one");
  feat->SetGeometryDirectly(new OGRPoint(1, 2));
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  poDS.reset();

  // Width-0 string fields without AUTOGROW share one
  // "string_default_max_width" dimension of the default width (80 when not
  // auto-growing), and values round-trip.
  {
    std::unique_ptr<GDALDataset> poReadDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    OGRLayer* rl = poReadDS->GetLayer(0);
    ASSERT_NE(nullptr, rl);
    std::unique_ptr<OGRFeature> rf(rl->GetNextFeature());
    ASSERT_NE(nullptr, rf);
    EXPECT_STREQ("zero", rf->GetFieldAsString("f_str0"));
    EXPECT_STREQ("one", rf->GetFieldAsString("f_str1"));
  }
  {
    int ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_open(filename.c_str(), NC_NOWRITE, &ncid));
    int width_dimid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_dimid(ncid, "string_default_max_width", &width_dimid));
    size_t dimlen = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ncid, width_dimid, &dimlen));
    EXPECT_EQ(80u, dimlen);
    int nvars = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_nvars(ncid, &nvars));
    int num_char_vars_on_dim = 0;
    for (int varid = 0; varid < nvars; ++varid) {
      nc_type vtype = NC_NAT;
      int ndims = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_vartype(ncid, varid, &vtype));
      ASSERT_EQ(NC_NOERR, nc_inq_varndims(ncid, varid, &ndims));
      if (vtype != NC_CHAR || ndims != 2) continue;
      int dimids[2] = {-1, -1};
      ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, varid, dimids));
      if (dimids[1] == width_dimid) ++num_char_vars_on_dim;
    }
    EXPECT_EQ(2, num_char_vars_on_dim);
    ASSERT_EQ(NC_NOERR, nc_close(ncid));
  }

  VSIUnlink(filename.c_str());
}

}  // namespace
}  // namespace autotest2
