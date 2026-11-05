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
// Tests for GPX layer (ogrgpxlayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpx/ogrgpxlayer.cpp
//   https://gdal.org/drivers/vector/gpx.html

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpx/ogr_gpx.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Ge;
using ::testing::HasSubstr;
using ::testing::NotNull;

// Subclass to trigger poGeom->getCoordinateDimension() == 0 checks in upstream
// code.
class ZeroDimPoint : public OGRPoint {
 public:
  int getCoordinateDimension() const override { return 0; }
  OGRPoint* clone() const override { return new ZeroDimPoint(*this); }
};

std::string ReadMemFile(const char* path) {
  vsi_l_offset nSize = 0;
  GByte* pabyData = VSIGetMemFileBuffer(path, &nSize, FALSE);
  if (pabyData == nullptr || nSize == 0) return "";
  return std::string(reinterpret_cast<const char*>(pabyData), nSize);
}

class OgrGpxLayerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterOGRGPX(); }
};

// ---------------------------------------------------------------------------
// 1. Constructor & Options
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, ConstructorOptionsClampingAndNonExistentFile) {
  // Test non-existent file in read mode: constructor logs failure and
  // GetNextFeature returns nullptr.
  OGRGPXDataSource ds;
  {
    WithQuietHandler quiet;
    OGRGPXLayer nonExistentLayer(
        "/vsimem/ogrgpxlayer_test/does_not_exist_file.gpx", "waypoints",
        GPX_WPT, &ds, false, nullptr);
    EXPECT_EQ(nullptr, nonExistentLayer.GetNextFeature());
    EXPECT_EQ(&ds, nonExistentLayer.GetDataset());
  }

  // Test N_MAX_LINKS clamping (-1 clamps to 2; 150 clamps to 100).
  const char* openOptionsMin[] = {"N_MAX_LINKS=-1", nullptr};
  OGRGPXLayer layerMin("/vsimem/ogrgpxlayer_test/dummy.gpx", "waypoints",
                       GPX_WPT, &ds, true, openOptionsMin);
  EXPECT_THAT(layerMin.GetLayerDefn()->GetFieldIndex("link2_href"), Ge(0));
  EXPECT_EQ(-1, layerMin.GetLayerDefn()->GetFieldIndex("link3_href"));

  const char* openOptionsMax[] = {"N_MAX_LINKS=150", nullptr};
  OGRGPXLayer layerMax("/vsimem/ogrgpxlayer_test/dummy.gpx", "waypoints",
                       GPX_WPT, &ds, true, openOptionsMax);
  EXPECT_THAT(layerMax.GetLayerDefn()->GetFieldIndex("link100_href"), Ge(0));
  EXPECT_EQ(-1, layerMax.GetLayerDefn()->GetFieldIndex("link101_href"));

  // Test SHORT_NAMES=YES on GPX_TRACK_POINT and GPX_ROUTE_POINT.
  const char* openOptionsShort[] = {"SHORT_NAMES=YES", nullptr};
  OGRGPXLayer layerTrkPtShort("/vsimem/ogrgpxlayer_test/dummy.gpx",
                              "track_points", GPX_TRACK_POINT, &ds, true,
                              openOptionsShort);
  EXPECT_THAT(layerTrkPtShort.GetLayerDefn()->GetFieldIndex("trksegid"), Ge(0));
  EXPECT_THAT(layerTrkPtShort.GetLayerDefn()->GetFieldIndex("trksegptid"),
              Ge(0));
  EXPECT_EQ(-1, layerTrkPtShort.GetLayerDefn()->GetFieldIndex("track_seg_id"));

  OGRGPXLayer layerRtePtShort("/vsimem/ogrgpxlayer_test/dummy.gpx",
                              "route_points", GPX_ROUTE_POINT, &ds, true,
                              openOptionsShort);
  EXPECT_THAT(layerRtePtShort.GetLayerDefn()->GetFieldIndex("rteptid"), Ge(0));
  EXPECT_EQ(-1,
            layerRtePtShort.GetLayerDefn()->GetFieldIndex("route_point_id"));
}

TEST_F(OgrGpxLayerTest, ConstructorGpx10Fields) {
  constexpr char kGpx10Path[] = "/vsimem/ogrgpxlayer_test/gpx10_layer_test.gpx";
  constexpr char kGpx10Data[] =
      "<gpx version=\"1.0\" creator=\"test\">\n"
      "  <wpt lat=\"10.0\" lon=\"20.0\">\n"
      "    <url>http://example.com</url>\n"
      "    <urlname>Example</urlname>\n"
      "  </wpt>\n"
      "  <trk><trkseg>\n"
      "    <trkpt lat=\"10.0\" lon=\"20.0\">\n"
      "      <course>180.5</course>\n"
      "      <speed>12.3</speed>\n"
      "    </trkpt>\n"
      "  </trkseg></trk>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kGpx10Path, kGpx10Data);

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kGpx10Path, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* poWpt = poDS->GetLayerByName("waypoints");
  ASSERT_THAT(poWpt, NotNull());
  EXPECT_THAT(poWpt->GetLayerDefn()->GetFieldIndex("url"), Ge(0));
  EXPECT_THAT(poWpt->GetLayerDefn()->GetFieldIndex("urlname"), Ge(0));

  OGRLayer* poTrkPt = poDS->GetLayerByName("track_points");
  ASSERT_THAT(poTrkPt, NotNull());
  EXPECT_THAT(poTrkPt->GetLayerDefn()->GetFieldIndex("course"), Ge(0));
  EXPECT_THAT(poTrkPt->GetLayerDefn()->GetFieldIndex("speed"), Ge(0));
}

// ---------------------------------------------------------------------------
// 2. Capabilities & CreateField
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, TestCapabilityAndCreateField) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/cap_test.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  // In write mode:
  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  const char* createOptions[] = {"GPX_USE_EXTENSIONS=YES", nullptr};
  GDALDatasetUniquePtr poDS(driver->Create(kPath, 0, 0, 0, GDT_Unknown,
                                           const_cast<char**>(createOptions)));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* layer = poDS->CreateLayer("waypoints", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(layer, NotNull());

  EXPECT_TRUE(layer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(layer->TestCapability(OLCCreateField));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCZGeometries));
  EXPECT_FALSE(layer->TestCapability("NonExistentCapability"));

  // Create an existing field (returns OGRERR_NONE without duplicating).
  const int initialCount = layer->GetLayerDefn()->GetFieldCount();
  OGRFieldDefn existingDefn("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&existingDefn));
  EXPECT_EQ(initialCount, layer->GetLayerDefn()->GetFieldCount());

  // Create new fields with extensions allowed.
  OGRFieldDefn intDefn("ogr_int_ext", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&intDefn));

  OGRFieldDefn realDefn("ogr_real_ext", OFTReal);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&realDefn));

  OGRFieldDefn strDefn("ogr_str space", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&strDefn));

  OGRFieldDefn nsDefn("ns_xml_ext", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&nsDefn));

  OGRFieldDefn garminDefn("gpxx_WaypointExtension", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&garminDefn));

  // Close write dataset.
  poDS.reset();

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poReadDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poReadDS, NotNull());
  OGRLayer* readLayer = poReadDS->GetLayerByName("waypoints");
  ASSERT_THAT(readLayer, NotNull());

  EXPECT_FALSE(readLayer->TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(readLayer->TestCapability(OLCCreateField));

  // Create a dataset without GPX_USE_EXTENSIONS to test failure of CreateField.
  constexpr char kNoExtPath[] = "/vsimem/ogrgpxlayer_test/no_ext_test.gpx";
  absl::Cleanup cleanupNoExt = [kNoExtPath] { VSIUnlink(kNoExtPath); };
  GDALDatasetUniquePtr poDSNoExt(
      driver->Create(kNoExtPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(poDSNoExt, NotNull());
  OGRLayer* layerNoExt =
      poDSNoExt->CreateLayer("waypoints", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(layerNoExt, NotNull());

  {
    WithQuietHandler quiet;
    OGRFieldDefn newDefn("custom_field", OFTString);
    EXPECT_EQ(OGRERR_FAILURE, layerNoExt->CreateField(&newDefn));
  }
}

// ---------------------------------------------------------------------------
// 3. Writing Features: Waypoints, Attributes, Extensions, Coordinate Checks
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, WriteWaypointsAttributesAndCoordinateValidity) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/write_wpt_test.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  const char* createOptions[] = {"GPX_USE_EXTENSIONS=YES", nullptr};
  GDALDatasetUniquePtr poDS(driver->Create(kPath, 0, 0, 0, GDT_Unknown,
                                           const_cast<char**>(createOptions)));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* layer = poDS->CreateLayer("waypoints", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(layer, NotNull());

  OGRFieldDefn intExt("ogr_int_ext", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&intExt));
  OGRFieldDefn realExt("ogr_real_ext", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&realExt));
  OGRFieldDefn nsExt("ns_xml_ext", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&nsExt));
  OGRFieldDefn garminExt("gpxx_WaypointExtension", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&garminExt));

  // Coordinate check failures and warnings (two calls for each static bool).
  // Invalid latitude (> 90 or < -90) logs error but proceeds to write and
  // returns OGRERR_NONE.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(10.0, 95.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(10.0, -95.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Out of range longitude triggers warning and clamping.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(190.0, 10.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(-190.0, 10.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Geometry errors: null geometry, non-point geometry, point empty.
  {
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetGeometry(nullptr);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&feat));
  }
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRLineString line;
    line.addPoint(0, 0);
    line.addPoint(1, 1);
    feat.SetGeometry(&line);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&feat));
  }
  {
    OGRFeature feat(layer->GetLayerDefn());
    ZeroDimPoint emptyPt;
    feat.SetGeometry(&emptyPt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&feat));
  }

  // Full feature with standard fields and XML extensions.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);

    feat.SetField("ele", 105.5);
    feat.SetField("time", "2023/04/01 12:00:00+00");
    feat.SetField("name", "TestWpt");
    feat.SetField("sat", 7);
    feat.SetField("link1_href", "http://google.com");
    feat.SetField("link1_text", "Google");
    feat.SetField("link1_type", "text/html");

    feat.SetField("ogr_int_ext", 42);
    feat.SetField("ogr_real_ext", 3.1415);
    feat.SetField("ns_xml_ext", "<sub a=\"1\">valid xml</sub>");
    feat.SetField("gpxx_WaypointExtension", "<garmin>GarminData</garmin>");

    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Test OGRGPX_GetUTF8String static bool bFirstTime with invalid UTF-8 (called
  // twice).
  const std::string invalidUtf8 = "<data>\xFF\xFE</data>";
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);
    feat.SetField("ns_xml_ext", invalidUtf8.c_str());
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);
    feat.SetField("ns_xml_ext", invalidUtf8.c_str());
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Test XML-like string with syntax error (<not xml < >) returning false in
  // OGRGPX_WriteXMLExtension.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);
    feat.SetField("ns_xml_ext", "<not xml < >");
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Test escaped XML (&lt;sub&gt;text&lt;/sub&gt;) succeeding.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);
    feat.SetField("ns_xml_ext", "&lt;sub&gt;escaped&lt;/sub&gt;");
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Test escaped XML with invalid XML content.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);
    feat.SetField("ns_xml_ext", "&lt;not xml &lt; &gt;");
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Test numeric field with leading spaces.
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(20.0, 10.0);
    feat.SetGeometry(&pt);
    feat.SetField("ogr_int_ext", "   999");
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Flush and verify XML contents.
  poDS.reset();
  const std::string written = ReadMemFile(kPath);
  EXPECT_THAT(written, HasSubstr("<wpt lat=\"95.0\""));
  EXPECT_THAT(written, HasSubstr("<wpt lat=\"10.0\" lon=\"-170.0\">"));
  EXPECT_THAT(written, HasSubstr("<ele>105.5</ele>"));
  EXPECT_THAT(written, HasSubstr("<name>TestWpt</name>"));
  EXPECT_THAT(written, HasSubstr("<link "
                                 "href=\"http://google.com\"><text>Google</"
                                 "text><type>text/html</type></link>"));
  EXPECT_THAT(written, HasSubstr("<ogr:int_ext>42</ogr:int_ext>"));
  EXPECT_THAT(written, HasSubstr("<ogr:real_ext>3.1415</ogr:real_ext>"));
  EXPECT_THAT(
      written,
      HasSubstr("<ns:xml_ext><sub a=\"1\">valid xml</sub></ns:xml_ext>"));
  EXPECT_THAT(
      written,
      HasSubstr(
          "xmlns:gpxx=\"http://www.garmin.com/xmlschemas/GpxExtensions/v3\""));
}

// ---------------------------------------------------------------------------
// 4. Writing Routes, Route Points, Tracks, Track Points & Order Validation
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, WriteRoutesAndOrderEnforcement) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/write_routes_test.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  GDALDatasetUniquePtr poDS(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* routes =
      poDS->CreateLayer("routes", nullptr, wkbLineString, nullptr);
  ASSERT_THAT(routes, NotNull());
  OGRLayer* waypoints =
      poDS->CreateLayer("waypoints", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(waypoints, NotNull());

  // Null geometry on route succeeds (<rte></rte>).
  {
    OGRFeature feat(routes->GetLayerDefn());
    feat.SetGeometry(nullptr);
    EXPECT_EQ(OGRERR_NONE, routes->CreateFeature(&feat));
  }

  // wkbLineString 2D
  {
    OGRFeature feat(routes->GetLayerDefn());
    OGRLineString line;
    line.addPoint(10.0, 20.0);
    line.addPoint(11.0, 21.0);
    feat.SetGeometry(&line);
    EXPECT_EQ(OGRERR_NONE, routes->CreateFeature(&feat));
  }

  // wkbLineString 25D (has ele)
  {
    OGRFeature feat(routes->GetLayerDefn());
    OGRLineString line;
    line.addPoint(10.0, 20.0, 50.0);
    line.addPoint(11.0, 21.0, 60.0);
    feat.SetGeometry(&line);
    EXPECT_EQ(OGRERR_NONE, routes->CreateFeature(&feat));
  }

  // wkbMultiLineString with 0 geometries
  {
    OGRFeature feat(routes->GetLayerDefn());
    OGRMultiLineString mls;
    feat.SetGeometry(&mls);
    EXPECT_EQ(OGRERR_NONE, routes->CreateFeature(&feat));
  }

  // wkbMultiLineString 25D with 1 geometry
  {
    OGRFeature feat(routes->GetLayerDefn());
    OGRMultiLineString mls;
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(10.0, 20.0, 75.0);
    mls.addGeometryDirectly(line.release());
    feat.SetGeometry(&mls);
    EXPECT_EQ(OGRERR_NONE, routes->CreateFeature(&feat));
  }

  // wkbMultiLineString with > 1 geometries fails.
  {
    OGRFeature feat(routes->GetLayerDefn());
    OGRMultiLineString mls;
    auto line1 = std::make_unique<OGRLineString>();
    line1->addPoint(10.0, 20.0);
    auto line2 = std::make_unique<OGRLineString>();
    line2->addPoint(11.0, 21.0);
    mls.addGeometryDirectly(line1.release());
    mls.addGeometryDirectly(line2.release());
    feat.SetGeometry(&mls);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routes->CreateFeature(&feat));
  }

  // Unsupported geometry (Point) on route fails.
  {
    OGRFeature feat(routes->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routes->CreateFeature(&feat));
  }

  // Writing waypoint after route fails.
  {
    OGRFeature feat(waypoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, waypoints->CreateFeature(&feat));
  }

  poDS.reset();
  const std::string written = ReadMemFile(kPath);
  EXPECT_THAT(written, HasSubstr("<rte>"));
  EXPECT_THAT(written, HasSubstr("</rte>"));
  EXPECT_THAT(written, HasSubstr("<ele>50.0</ele>"));
}

TEST_F(OgrGpxLayerTest, WriteRoutePointsAndTransitionToRoutes) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/write_rtept_test.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  GDALDatasetUniquePtr poDS(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* routePoints =
      poDS->CreateLayer("route_points", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(routePoints, NotNull());
  OGRLayer* routes =
      poDS->CreateLayer("routes", nullptr, wkbLineString, nullptr);
  ASSERT_THAT(routes, NotNull());

  // Error paths: null geometry, non-point, point empty, unset/negative
  // route_fid.
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    feat.SetGeometry(nullptr);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routePoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    ZeroDimPoint emptyPt;
    feat.SetGeometry(&emptyPt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routePoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routePoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("route_fid", -1);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routePoints->CreateFeature(&feat));
  }

  // Valid route points: fid 0 point 1, fid 0 point 2, fid 1 point 3.
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("route_fid", 0);
    feat.SetField("route_name", "Route0");
    EXPECT_EQ(OGRERR_NONE, routePoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    OGRPoint pt(10.1, 20.1);
    feat.SetGeometry(&pt);
    feat.SetField("route_fid", 0);
    EXPECT_EQ(OGRERR_NONE, routePoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    OGRPoint pt(11.0, 21.0);
    feat.SetGeometry(&pt);
    feat.SetField("route_fid", 1);
    feat.SetField("route_name", "Route1");
    EXPECT_EQ(OGRERR_NONE, routePoints->CreateFeature(&feat));
  }

  // Now writing to routes triggers closing previous route_points (<rte>).
  {
    OGRFeature feat(routes->GetLayerDefn());
    feat.SetGeometry(nullptr);
    EXPECT_EQ(OGRERR_NONE, routes->CreateFeature(&feat));
  }

  poDS.reset();
  const std::string written = ReadMemFile(kPath);
  EXPECT_THAT(written, HasSubstr("<name>Route0</name>"));
  EXPECT_THAT(written, HasSubstr("<name>Route1</name>"));
}

TEST_F(OgrGpxLayerTest, WriteTracksAndTrackPoints) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/write_tracks_test.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  GDALDatasetUniquePtr poDS(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* tracks =
      poDS->CreateLayer("tracks", nullptr, wkbMultiLineString, nullptr);
  ASSERT_THAT(tracks, NotNull());
  OGRLayer* waypoints =
      poDS->CreateLayer("waypoints", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(waypoints, NotNull());
  OGRLayer* routes =
      poDS->CreateLayer("routes", nullptr, wkbLineString, nullptr);
  ASSERT_THAT(routes, NotNull());
  OGRLayer* trackPoints =
      poDS->CreateLayer("track_points", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(trackPoints, NotNull());

  // tracks geometry types:
  // Null geometry succeeds (<trk></trk>).
  {
    OGRFeature feat(tracks->GetLayerDefn());
    feat.SetGeometry(nullptr);
    EXPECT_EQ(OGRERR_NONE, tracks->CreateFeature(&feat));
  }

  // wkbLineString 2D
  {
    OGRFeature feat(tracks->GetLayerDefn());
    OGRLineString line;
    line.addPoint(10.0, 20.0);
    line.addPoint(11.0, 21.0);
    feat.SetGeometry(&line);
    EXPECT_EQ(OGRERR_NONE, tracks->CreateFeature(&feat));
  }

  // wkbLineString 25D
  {
    OGRFeature feat(tracks->GetLayerDefn());
    OGRLineString line;
    line.addPoint(10.0, 20.0, 100.0);
    line.addPoint(11.0, 21.0, 110.0);
    feat.SetGeometry(&line);
    EXPECT_EQ(OGRERR_NONE, tracks->CreateFeature(&feat));
  }

  // wkbMultiLineString 2D with 2 lines
  {
    OGRFeature feat(tracks->GetLayerDefn());
    OGRMultiLineString mls;
    auto l1 = std::make_unique<OGRLineString>();
    l1->addPoint(10.0, 20.0);
    auto l2 = std::make_unique<OGRLineString>();
    l2->addPoint(11.0, 21.0);
    mls.addGeometryDirectly(l1.release());
    mls.addGeometryDirectly(l2.release());
    feat.SetGeometry(&mls);
    EXPECT_EQ(OGRERR_NONE, tracks->CreateFeature(&feat));
  }

  // wkbMultiLineString 25D with 2 lines
  {
    OGRFeature feat(tracks->GetLayerDefn());
    OGRMultiLineString mls;
    auto l1 = std::make_unique<OGRLineString>();
    l1->addPoint(10.0, 20.0, 200.0);
    auto l2 = std::make_unique<OGRLineString>();
    l2->addPoint(11.0, 21.0, 210.0);
    mls.addGeometryDirectly(l1.release());
    mls.addGeometryDirectly(l2.release());
    feat.SetGeometry(&mls);
    EXPECT_EQ(OGRERR_NONE, tracks->CreateFeature(&feat));
  }

  // Unsupported geometry (Point) on tracks fails.
  {
    OGRFeature feat(tracks->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, tracks->CreateFeature(&feat));
  }

  // Order errors after track written:
  // Writing waypoint or route after track fails.
  {
    OGRFeature feat(waypoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, waypoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(routes->GetLayerDefn());
    feat.SetGeometry(nullptr);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, routes->CreateFeature(&feat));
  }

  // Track points:
  // Error paths: null geom, empty pt, unset/negative track_fid/track_seg_id.
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    feat.SetGeometry(nullptr);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    ZeroDimPoint emptyPt;
    feat.SetGeometry(&emptyPt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", -1);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", 0);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", 0);
    feat.SetField("track_seg_id", -1);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, trackPoints->CreateFeature(&feat));
  }

  // Valid track points transitions:
  // fid 0 seg 0, then fid 0 seg 1 (switches seg), then fid 1 seg 0 (switches
  // trk).
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", 0);
    feat.SetField("track_seg_id", 0);
    feat.SetField("track_name", "Track0");
    EXPECT_EQ(OGRERR_NONE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(10.1, 20.1);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", 0);
    feat.SetField("track_seg_id", 1);
    EXPECT_EQ(OGRERR_NONE, trackPoints->CreateFeature(&feat));
  }
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(11.0, 21.0);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", 1);
    feat.SetField("track_seg_id", 0);
    feat.SetField("track_name", "Track1");
    EXPECT_EQ(OGRERR_NONE, trackPoints->CreateFeature(&feat));
  }

  // Now writing a track feature triggers closing the open track_points trk and
  // trkseg.
  {
    OGRFeature feat(tracks->GetLayerDefn());
    feat.SetGeometry(nullptr);
    EXPECT_EQ(OGRERR_NONE, tracks->CreateFeature(&feat));
  }

  poDS.reset();
  const std::string written = ReadMemFile(kPath);
  EXPECT_THAT(written, HasSubstr("<trk>"));
  EXPECT_THAT(written, HasSubstr("</trk>"));
  EXPECT_THAT(written, HasSubstr("</trkseg>"));
}

TEST_F(OgrGpxLayerTest, WriteRoutePointsTransitionToTrackPoints) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/write_transition_test.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  GDALDatasetUniquePtr poDS(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* routePoints =
      poDS->CreateLayer("route_points", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(routePoints, NotNull());
  OGRLayer* trackPoints =
      poDS->CreateLayer("track_points", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(trackPoints, NotNull());

  // Write route point.
  {
    OGRFeature feat(routePoints->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    feat.SetField("route_fid", 0);
    EXPECT_EQ(OGRERR_NONE, routePoints->CreateFeature(&feat));
  }

  // Transition directly to track point (closes open <rte>).
  {
    OGRFeature feat(trackPoints->GetLayerDefn());
    OGRPoint pt(11.0, 21.0);
    feat.SetGeometry(&pt);
    feat.SetField("track_fid", 0);
    feat.SetField("track_seg_id", 0);
    EXPECT_EQ(OGRERR_NONE, trackPoints->CreateFeature(&feat));
  }
}

// ---------------------------------------------------------------------------
// 5. Reading Features: All Layers, Standard Fields, Links, Queue, EOF, Reset
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, ReadWaypointsAllFieldsAndLinks) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/read_wpt_full.gpx";
  constexpr char kGpxData[] =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <wpt lat=\"10.5\" lon=\"20.5\">\n"
      "    <ele>123.4</ele>\n"
      "    <time>2023-01-02T03:04:05Z</time>\n"
      "    <magvar>1.25</magvar>\n"
      "    <geoidheight>2.5</geoidheight>\n"
      "    <name>Point Alpha</name>\n"
      "    <cmt>Comment Alpha</cmt>\n"
      "    <desc>Desc Alpha</desc>\n"
      "    <src>Src Alpha</src>\n"
      "    <link "
      "href=\"http://link1\"><text>Text1</text><type>Type1</type></link>\n"
      "    <link "
      "href=\"http://link2\"><text>Text2</text><type>Type2</type></link>\n"
      "    <link href=\"http://link3\"><text>Text3</text></link>\n"
      "    <link href=\"http://link4\"><type>Type4</type></link>\n"
      "    <sym>Waypoint</sym>\n"
      "    <type>Marker</type>\n"
      "    <fix>3d</fix>\n"
      "    <sat>8</sat>\n"
      "    <hdop>1.1</hdop>\n"
      "    <vdop>1.2</vdop>\n"
      "    <pdop>1.5</pdop>\n"
      "    <ageofdgpsdata>4.5</ageofdgpsdata>\n"
      "    <dgpsid>12</dgpsid>\n"
      "  </wpt>\n"
      "  <wpt lat=\"11.0\" lon=\"21.0\">\n"
      "    <time>time</time>\n"  // Upstream bug: checks m_osSubElementValue ==
                                 // "time"
      "  </wpt>\n"
      "  <wpt>\n"  // Missing lat/lon -> skipped
      "    <name>NoCoords</name>\n"
      "  </wpt>\n"
      "  <wpt lat=\"12.0\" lon=\"22.0\">\n"
      "    <name>Point Bravo</name>\n"
      "  </wpt>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, kGpxData);

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* layer = poDS->GetLayerByName("waypoints");
  ASSERT_THAT(layer, NotNull());

  // Feature 1: Point Alpha (quieted for static once link-limit warning and
  // dateTime warning)
  std::unique_ptr<OGRFeature> feat1;
  {
    WithQuietHandler quiet;
    feat1 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  }
  ASSERT_THAT(feat1, NotNull());
  EXPECT_DOUBLE_EQ(123.4, feat1->GetFieldAsDouble("ele"));
  EXPECT_STREQ("Point Alpha", feat1->GetFieldAsString("name"));
  EXPECT_STREQ("Comment Alpha", feat1->GetFieldAsString("cmt"));
  EXPECT_STREQ("http://link1", feat1->GetFieldAsString("link1_href"));
  EXPECT_STREQ("Text1", feat1->GetFieldAsString("link1_text"));
  EXPECT_STREQ("Type1", feat1->GetFieldAsString("link1_type"));
  EXPECT_STREQ("http://link2", feat1->GetFieldAsString("link2_href"));
  EXPECT_EQ(8, feat1->GetFieldAsInteger("sat"));
  ASSERT_THAT(feat1->GetGeometryRef(), NotNull());
  EXPECT_DOUBLE_EQ(20.5, feat1->GetGeometryRef()->toPoint()->getX());
  EXPECT_DOUBLE_EQ(10.5, feat1->GetGeometryRef()->toPoint()->getY());

  // Feature 2: Invalid dateTime <time>time</time>
  std::unique_ptr<OGRFeature> feat2;
  {
    WithQuietHandler quiet;
    feat2 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  }
  ASSERT_THAT(feat2, NotNull());

  // Feature 3: Point Bravo (NoCoords was skipped)
  std::unique_ptr<OGRFeature> feat3(layer->GetNextFeature());
  ASSERT_THAT(feat3, NotNull());
  EXPECT_STREQ("Point Bravo", feat3->GetFieldAsString("name"));

  // EOF
  EXPECT_EQ(nullptr, layer->GetNextFeature());
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // ResetReading
  layer->ResetReading();
  std::unique_ptr<OGRFeature> featReset;
  {
    WithQuietHandler quiet;
    featReset = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  }
  ASSERT_THAT(featReset, NotNull());
  EXPECT_STREQ("Point Alpha", featReset->GetFieldAsString("name"));
}

TEST_F(OgrGpxLayerTest, ReadRoutesAndRoutePoints) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/read_routes.gpx";
  constexpr char kGpxData[] =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <rte>\n"
      "    <name>Route A</name>\n"
      "    <number>1</number>\n"
      "    <rtept lat=\"10.0\" lon=\"20.0\"/>\n"
      "    <rtept/>\n"  // Missing lat/lon -> consumed id, but skipped
      "    <rtept lat=\"11.0\" lon=\"21.0\"/>\n"
      "  </rte>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, kGpxData);

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* routes = poDS->GetLayerByName("routes");
  ASSERT_THAT(routes, NotNull());
  std::unique_ptr<OGRFeature> rteFeat(routes->GetNextFeature());
  ASSERT_THAT(rteFeat, NotNull());
  EXPECT_STREQ("Route A", rteFeat->GetFieldAsString("name"));
  EXPECT_EQ(1, rteFeat->GetFieldAsInteger("number"));
  ASSERT_THAT(rteFeat->GetGeometryRef(), NotNull());
  EXPECT_EQ(wkbLineString, rteFeat->GetGeometryRef()->getGeometryType());
  EXPECT_EQ(2, rteFeat->GetGeometryRef()->toLineString()->getNumPoints());

  OGRLayer* routePoints = poDS->GetLayerByName("route_points");
  ASSERT_THAT(routePoints, NotNull());
  std::unique_ptr<OGRFeature> pt1(routePoints->GetNextFeature());
  ASSERT_THAT(pt1, NotNull());
  EXPECT_EQ(0, pt1->GetFieldAsInteger("route_fid"));
  EXPECT_EQ(0, pt1->GetFieldAsInteger("route_point_id"));

  std::unique_ptr<OGRFeature> pt2(routePoints->GetNextFeature());
  ASSERT_THAT(pt2, NotNull());
  EXPECT_EQ(0, pt2->GetFieldAsInteger("route_fid"));
  EXPECT_EQ(2, pt2->GetFieldAsInteger("route_point_id"));

  EXPECT_EQ(nullptr, routePoints->GetNextFeature());
}

TEST_F(OgrGpxLayerTest, ReadTracksAndTrackPoints) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/read_tracks.gpx";
  constexpr char kGpxData[] =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <trk>\n"
      "    <name>Track A</name>\n"
      "    <number>2</number>\n"
      "    <trkseg>\n"
      "      <trkpt lat=\"10.0\" lon=\"20.0\"/>\n"
      "      <trkpt/>\n"  // Skipped
      "      <trkpt lat=\"10.5\" lon=\"20.5\"/>\n"
      "    </trkseg>\n"
      "    <trkseg>\n"
      "      <trkpt lat=\"11.0\" lon=\"21.0\"/>\n"
      "    </trkseg>\n"
      "  </trk>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, kGpxData);

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* tracks = poDS->GetLayerByName("tracks");
  ASSERT_THAT(tracks, NotNull());
  std::unique_ptr<OGRFeature> trkFeat(tracks->GetNextFeature());
  ASSERT_THAT(trkFeat, NotNull());
  EXPECT_STREQ("Track A", trkFeat->GetFieldAsString("name"));
  ASSERT_THAT(trkFeat->GetGeometryRef(), NotNull());
  EXPECT_EQ(wkbMultiLineString, trkFeat->GetGeometryRef()->getGeometryType());
  EXPECT_EQ(2,
            trkFeat->GetGeometryRef()->toMultiLineString()->getNumGeometries());

  OGRLayer* trackPoints = poDS->GetLayerByName("track_points");
  ASSERT_THAT(trackPoints, NotNull());
  std::unique_ptr<OGRFeature> pt1(trackPoints->GetNextFeature());
  ASSERT_THAT(pt1, NotNull());
  EXPECT_EQ(0, pt1->GetFieldAsInteger("track_fid"));
  EXPECT_EQ(0, pt1->GetFieldAsInteger("track_seg_id"));
  EXPECT_EQ(0, pt1->GetFieldAsInteger("track_seg_point_id"));

  std::unique_ptr<OGRFeature> pt2(trackPoints->GetNextFeature());
  ASSERT_THAT(pt2, NotNull());
  EXPECT_EQ(0, pt2->GetFieldAsInteger("track_fid"));
  EXPECT_EQ(0, pt2->GetFieldAsInteger("track_seg_id"));
  EXPECT_EQ(2, pt2->GetFieldAsInteger("track_seg_point_id"));

  std::unique_ptr<OGRFeature> pt3(trackPoints->GetNextFeature());
  ASSERT_THAT(pt3, NotNull());
  EXPECT_EQ(0, pt3->GetFieldAsInteger("track_fid"));
  EXPECT_EQ(1, pt3->GetFieldAsInteger("track_seg_id"));
  EXPECT_EQ(0, pt3->GetFieldAsInteger("track_seg_point_id"));

  EXPECT_EQ(nullptr, trackPoints->GetNextFeature());
}

// ---------------------------------------------------------------------------
// 6. 2.5D Elevation Parsing
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, Elevation25DParsing) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/ele_25d_test.gpx";
  constexpr char kGpxData[] =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <wpt lat=\"10.0\" lon=\"20.0\"><ele>123.4</ele></wpt>\n"
      "  <rte>\n"
      "    <rtept lat=\"1.0\" lon=\"2.0\"><ele>50.0</ele></rtept>\n"
      "    <rtept lat=\"3.0\" lon=\"4.0\"><ele>60.0</ele></rtept>\n"
      "  </rte>\n"
      "  <trk><trkseg>\n"
      "    <trkpt lat=\"5.0\" lon=\"6.0\"><ele>70.0</ele></trkpt>\n"
      "  </trkseg></trk>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, kGpxData);

  const char* openOptions[] = {"ELE_AS_25D=YES", nullptr};
  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers, openOptions));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* wpt = poDS->GetLayerByName("waypoints");
  ASSERT_THAT(wpt, NotNull());
  std::unique_ptr<OGRFeature> wptFeat(wpt->GetNextFeature());
  ASSERT_THAT(wptFeat, NotNull());
  EXPECT_EQ(wkbPoint25D, wptFeat->GetGeometryRef()->getGeometryType());
  EXPECT_DOUBLE_EQ(123.4, wptFeat->GetGeometryRef()->toPoint()->getZ());

  OGRLayer* rte = poDS->GetLayerByName("routes");
  ASSERT_THAT(rte, NotNull());
  std::unique_ptr<OGRFeature> rteFeat(rte->GetNextFeature());
  ASSERT_THAT(rteFeat, NotNull());
  EXPECT_EQ(wkbLineString25D, rteFeat->GetGeometryRef()->getGeometryType());
  EXPECT_DOUBLE_EQ(50.0, rteFeat->GetGeometryRef()->toLineString()->getZ(0));
  EXPECT_DOUBLE_EQ(60.0, rteFeat->GetGeometryRef()->toLineString()->getZ(1));

  OGRLayer* trk = poDS->GetLayerByName("tracks");
  ASSERT_THAT(trk, NotNull());
  std::unique_ptr<OGRFeature> trkFeat(trk->GetNextFeature());
  ASSERT_THAT(trkFeat, NotNull());
  EXPECT_EQ(wkbMultiLineString25D,
            trkFeat->GetGeometryRef()->getGeometryType());
  auto* mls = trkFeat->GetGeometryRef()->toMultiLineString();
  ASSERT_THAT(mls, NotNull());
  ASSERT_THAT(mls->getGeometryRef(0), NotNull());
  EXPECT_DOUBLE_EQ(70.0, mls->getGeometryRef(0)->toLineString()->getZ(0));
}

// ---------------------------------------------------------------------------
// 7. Spatial and Attribute Filtering
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, SpatialAndAttributeFiltering) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/filter_test.gpx";
  constexpr char kGpxData[] =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <wpt lat=\"10.0\" lon=\"20.0\"><name>Match</name></wpt>\n"
      "  <wpt lat=\"50.0\" lon=\"60.0\"><name>NoMatch</name></wpt>\n"
      "  <rte><name>MatchRte</name>\n"
      "    <rtept lat=\"10.0\" lon=\"20.0\"/>\n"
      "  </rte>\n"
      "  <rte><name>NoMatchRte</name>\n"
      "    <rtept lat=\"50.0\" lon=\"60.0\"/>\n"
      "  </rte>\n"
      "  <trk><name>MatchTrk</name><trkseg>\n"
      "    <trkpt lat=\"10.0\" lon=\"20.0\"/>\n"
      "  </trkseg></trk>\n"
      "  <trk><name>NoMatchTrk</name><trkseg>\n"
      "    <trkpt lat=\"50.0\" lon=\"60.0\"/>\n"
      "  </trkseg></trk>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, kGpxData);

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poDS, NotNull());

  // Spatial filter on waypoints
  OGRLayer* wpt = poDS->GetLayerByName("waypoints");
  ASSERT_THAT(wpt, NotNull());
  wpt->SetSpatialFilterRect(19.0, 9.0, 21.0, 11.0);
  std::unique_ptr<OGRFeature> wf(wpt->GetNextFeature());
  ASSERT_THAT(wf, NotNull());
  EXPECT_STREQ("Match", wf->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, wpt->GetNextFeature());

  // Attribute filter on waypoints
  wpt->SetSpatialFilter(nullptr);
  wpt->ResetReading();
  wpt->SetAttributeFilter("name = 'NoMatch'");
  wf = std::unique_ptr<OGRFeature>(wpt->GetNextFeature());
  ASSERT_THAT(wf, NotNull());
  EXPECT_STREQ("NoMatch", wf->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, wpt->GetNextFeature());

  // Spatial filter on routes
  OGRLayer* rte = poDS->GetLayerByName("routes");
  ASSERT_THAT(rte, NotNull());
  rte->SetSpatialFilterRect(19.0, 9.0, 21.0, 11.0);
  std::unique_ptr<OGRFeature> rf(rte->GetNextFeature());
  ASSERT_THAT(rf, NotNull());
  EXPECT_STREQ("MatchRte", rf->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, rte->GetNextFeature());

  // Attribute filter on routes
  rte->SetSpatialFilter(nullptr);
  rte->ResetReading();
  rte->SetAttributeFilter("name = 'NoMatchRte'");
  rf = std::unique_ptr<OGRFeature>(rte->GetNextFeature());
  ASSERT_THAT(rf, NotNull());
  EXPECT_STREQ("NoMatchRte", rf->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, rte->GetNextFeature());

  // Spatial filter on tracks
  OGRLayer* trk = poDS->GetLayerByName("tracks");
  ASSERT_THAT(trk, NotNull());
  trk->SetSpatialFilterRect(19.0, 9.0, 21.0, 11.0);
  std::unique_ptr<OGRFeature> tf(trk->GetNextFeature());
  ASSERT_THAT(tf, NotNull());
  EXPECT_STREQ("MatchTrk", tf->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, trk->GetNextFeature());

  // Attribute filter on tracks
  trk->SetSpatialFilter(nullptr);
  trk->ResetReading();
  trk->SetAttributeFilter("name = 'NoMatchTrk'");
  tf = std::unique_ptr<OGRFeature>(trk->GetNextFeature());
  ASSERT_THAT(tf, NotNull());
  EXPECT_STREQ("NoMatchTrk", tf->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, trk->GetNextFeature());
}

// ---------------------------------------------------------------------------
// 8. Extensions Schema Parsing & Type Promotions
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, LoadExtensionsSchemaAndTypePromotion) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/ext_schema_test.gpx";
  constexpr char kGpxData[] =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <wpt lat=\"1.0\" lon=\"2.0\">\n"
      "    <extensions>\n"
      "      <ns:int_val>  +42</ns:int_val>\n"
      "      <ns:promo_field>10</ns:promo_field>\n"
      "      <ns:sign_mid>1e+2</ns:sign_mid>\n"
      "      <name>standard_override</name>\n"
      "      <ns:complex>\n<child attr=\"val\" "
      "attr2=\"val2\">inner</child><empty/></ns:complex>\n"
      "    </extensions>\n"
      "  </wpt>\n"
      "  <wpt lat=\"3.0\" lon=\"4.0\">\n"
      "    <extensions>\n"
      "      <ns:promo_field>10.5</ns:promo_field>\n"
      "    </extensions>\n"
      "  </wpt>\n"
      "  <wpt lat=\"5.0\" lon=\"6.0\">\n"
      "    <extensions>\n"
      "      <ns:promo_field>10.5text</ns:promo_field>\n"
      "    </extensions>\n"
      "  </wpt>\n"
      "  <rte><extensions><ns:rte_field>100</ns:rte_field></extensions></rte>\n"
      "  <trk><extensions><ns:trk_field>200</ns:trk_field></extensions></trk>\n"
      "  <trk><trkseg><trkpt lat=\"1\" "
      "lon=\"2\"><extensions><ns:trkpt_field>300</ns:trkpt_field></"
      "extensions></trkpt></trkseg></trk>\n"
      "  <rte><rtept lat=\"1\" "
      "lon=\"2\"><extensions><ns:rtept_field>400</ns:rtept_field></"
      "extensions></rtept></rte>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, kGpxData);

  CPLSetConfigOption("GPX_USE_EXTENSIONS", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GPX_USE_EXTENSIONS", nullptr);
  };

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS(
      GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* wpt = poDS->GetLayerByName("waypoints");
  ASSERT_THAT(wpt, NotNull());

  // Check field definitions inferred during LoadExtensionsSchema:
  // ns:int_val should be OFTInteger
  int iInt = wpt->GetLayerDefn()->GetFieldIndex("ns_int_val");
  ASSERT_THAT(iInt, Ge(0));
  EXPECT_EQ(OFTInteger, wpt->GetLayerDefn()->GetFieldDefn(iInt)->GetType());

  // ns:sign_mid: 1e+2 has non-digit chars so OGRGPXIsInt is false -> promoted
  // to OFTReal
  int iSign = wpt->GetLayerDefn()->GetFieldIndex("ns_sign_mid");
  ASSERT_THAT(iSign, Ge(0));
  EXPECT_EQ(OFTReal, wpt->GetLayerDefn()->GetFieldDefn(iSign)->GetType());

  // ns:promo_field started 10 (int), then 10.5 (real), then 10.5text (string)
  // -> OFTString
  int iPromo = wpt->GetLayerDefn()->GetFieldIndex("ns_promo_field");
  ASSERT_THAT(iPromo, Ge(0));
  EXPECT_EQ(OFTString, wpt->GetLayerDefn()->GetFieldDefn(iPromo)->GetType());

  // ns:complex has nested XML
  int iComplex = wpt->GetLayerDefn()->GetFieldIndex("ns_complex");
  ASSERT_THAT(iComplex, Ge(0));
  EXPECT_EQ(OFTString, wpt->GetLayerDefn()->GetFieldDefn(iComplex)->GetType());

  // Read feature values
  std::unique_ptr<OGRFeature> feat1(wpt->GetNextFeature());
  ASSERT_THAT(feat1, NotNull());
  EXPECT_EQ(42, feat1->GetFieldAsInteger(iInt));
  EXPECT_THAT(feat1->GetFieldAsString(iComplex),
              HasSubstr("<child attr=\"val\""));

  // Check extensions on routes, tracks, route_points, track_points
  OGRLayer* rte = poDS->GetLayerByName("routes");
  ASSERT_THAT(rte, NotNull());
  EXPECT_THAT(rte->GetLayerDefn()->GetFieldIndex("ns_rte_field"), Ge(0));

  OGRLayer* trk = poDS->GetLayerByName("tracks");
  ASSERT_THAT(trk, NotNull());
  EXPECT_THAT(trk->GetLayerDefn()->GetFieldIndex("ns_trk_field"), Ge(0));

  OGRLayer* rtePt = poDS->GetLayerByName("route_points");
  ASSERT_THAT(rtePt, NotNull());
  EXPECT_THAT(rtePt->GetLayerDefn()->GetFieldIndex("ns_rtept_field"), Ge(0));

  OGRLayer* trkPt = poDS->GetLayerByName("track_points");
  ASSERT_THAT(trkPt, NotNull());
  EXPECT_THAT(trkPt->GetLayerDefn()->GetFieldIndex("ns_trkpt_field"), Ge(0));
}

TEST_F(OgrGpxLayerTest, LoadExtensionsSchemaMaxFieldsLimit) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/max_fields_test.gpx";

  // Create a GPX file with 85 extension fields inside wpt to hit the 100 fields
  // limit.
  std::string xml =
      "<gpx version=\"1.1\" creator=\"test\">\n"
      "  <wpt lat=\"1.0\" lon=\"2.0\">\n"
      "    <extensions>\n";
  for (int i = 0; i < 85; ++i) {
    xml += "      <field_" + std::to_string(i) + ">1</field_" +
           std::to_string(i) + ">\n";
  }
  xml +=
      "    </extensions>\n"
      "  </wpt>\n"
      "</gpx>\n";
  VsiMemTempWrapper memFile(kPath, xml);

  CPLSetConfigOption("GPX_USE_EXTENSIONS", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GPX_USE_EXTENSIONS", nullptr);
  };

  const char* allowedDrivers[] = {"GPX", nullptr};
  GDALDatasetUniquePtr poDS;
  {
    WithQuietHandler quiet;
    poDS = GDALDatasetUniquePtr(
        GDALDataset::Open(kPath, GDAL_OF_VECTOR, allowedDrivers));
  }
  ASSERT_THAT(poDS, NotNull());
  OGRLayer* wpt = poDS->GetLayerByName("waypoints");
  ASSERT_THAT(wpt, NotNull());
  EXPECT_EQ(100, wpt->GetLayerDefn()->GetFieldCount());
}

// ---------------------------------------------------------------------------
// 9. Parser Error Handling & DoS Safety Limits
// ---------------------------------------------------------------------------

TEST_F(OgrGpxLayerTest, WriteModeGetNextFeatureFails) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/write_mode_read_fail.gpx";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_THAT(driver, NotNull());
  GDALDatasetUniquePtr poDS(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(poDS, NotNull());

  OGRLayer* layer = poDS->CreateLayer("waypoints", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(layer, NotNull());

  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, layer->GetNextFeature());
}

TEST_F(OgrGpxLayerTest, MalformedXmlInGetNextFeature) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/malformed_get_next.gpx";
  VsiMemTempWrapper memFile(
      kPath, "<gpx version=\"1.1\"><wpt lat=\"1\" lon=\"2\"><unclosed");

  OGRGPXDataSource ds;
  OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, layer.GetNextFeature());
}

TEST_F(OgrGpxLayerTest, ElementTooLargeInGetNextFeature) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/element_too_large.gpx";

  std::string huge(105000, 'x');
  std::string xml = "<gpx version=\"1.1\"><wpt lat=\"1\" lon=\"2\"><desc>" +
                    huge + "</desc></wpt></gpx>";
  VsiMemTempWrapper memFile(kPath, xml);

  OGRGPXDataSource ds;
  OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, layer.GetNextFeature());
}

TEST_F(OgrGpxLayerTest, WithoutEventCounterInGetNextFeature) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/without_event.gpx";

  // An XML comment containing > 81920 spaces (10 buffer chunks without event
  // callbacks)
  std::string spaces(85000, ' ');
  std::string xml = "<gpx version=\"1.1\"><!-- " + spaces +
                    " --><wpt lat=\"1\" lon=\"2\"/></gpx>";
  VsiMemTempWrapper memFile(kPath, xml);

  OGRGPXDataSource ds;
  OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, layer.GetNextFeature());
}

TEST_F(OgrGpxLayerTest, MillionLaughInGetNextFeature) {
  constexpr char kPath[] =
      "/vsimem/ogrgpxlayer_test/million_laugh_get_next.gpx";

  constexpr char kLaughs[] =
      "<!DOCTYPE gpx [\n"
      "  <!ENTITY a \"x\">\n"
      "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
      "  <!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">\n"
      "  <!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">\n"
      "  <!ENTITY e \"&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;\">\n"
      "]>\n"
      "<gpx version=\"1.1\"><wpt lat=\"1.0\" "
      "lon=\"2.0\"><desc>&e;</desc></wpt></gpx>";
  VsiMemTempWrapper memFile(kPath, kLaughs);

  OGRGPXDataSource ds;
  OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, layer.GetNextFeature());
}

TEST_F(OgrGpxLayerTest, MalformedXmlInLoadExtensionsSchema) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/malformed_schema.gpx";
  VsiMemTempWrapper memFile(
      kPath, "<gpx version=\"1.1\"><wpt lat=\"1\" lon=\"2\"><extensions><bad");

  CPLSetConfigOption("GPX_USE_EXTENSIONS", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GPX_USE_EXTENSIONS", nullptr);
  };

  OGRGPXDataSource ds;
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("XML parsing of GPX file failed"));
}

TEST_F(OgrGpxLayerTest, ElementTooLargeInLoadExtensionsSchema) {
  constexpr char kPath[] =
      "/vsimem/ogrgpxlayer_test/element_too_large_schema.gpx";

  std::string huge(105000, 'y');
  std::string xml =
      "<gpx version=\"1.1\"><wpt lat=\"1\" lon=\"2\"><extensions><big>" + huge +
      "</big></extensions></wpt></gpx>";
  VsiMemTempWrapper memFile(kPath, xml);

  CPLSetConfigOption("GPX_USE_EXTENSIONS", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GPX_USE_EXTENSIONS", nullptr);
  };

  OGRGPXDataSource ds;
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("parsing aborted"));
}

TEST_F(OgrGpxLayerTest, WithoutEventCounterInLoadExtensionsSchema) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/without_event_schema.gpx";

  std::string spaces(85000, ' ');
  std::string xml =
      "<gpx version=\"1.1\"><!-- " + spaces +
      " --><wpt lat=\"1\" "
      "lon=\"2\"><extensions><ext>1</ext></extensions></wpt></gpx>";
  VsiMemTempWrapper memFile(kPath, xml);

  CPLSetConfigOption("GPX_USE_EXTENSIONS", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GPX_USE_EXTENSIONS", nullptr);
  };

  OGRGPXDataSource ds;
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("Too much data inside one element. File probably corrupted"));
}

TEST_F(OgrGpxLayerTest, MillionLaughInLoadExtensionsSchema) {
  constexpr char kPath[] = "/vsimem/ogrgpxlayer_test/million_laugh_schema.gpx";

  constexpr char kLaughs[] =
      "<!DOCTYPE gpx [\n"
      "  <!ENTITY a \"x\">\n"
      "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
      "  <!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">\n"
      "  <!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">\n"
      "  <!ENTITY e \"&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;\">\n"
      "]>\n"
      "<gpx version=\"1.1\"><wpt lat=\"1.0\" "
      "lon=\"2.0\"><extensions><ext>&e;</ext></extensions></wpt></gpx>";
  VsiMemTempWrapper memFile(kPath, kLaughs);

  CPLSetConfigOption("GPX_USE_EXTENSIONS", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GPX_USE_EXTENSIONS", nullptr);
  };

  OGRGPXDataSource ds;
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    OGRGPXLayer layer(kPath, "waypoints", GPX_WPT, &ds, false, nullptr);
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("parsing aborted"));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Lines 885-886 in ogrgpxlayer.cpp are unreachable due to
// upstream bug checking m_osSubElementValue == "time" on OFTDateTime fields.
