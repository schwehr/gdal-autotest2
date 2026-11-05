// Copyright 2017 Google Inc. All Rights Reserved.
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
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gdal/ogr/gml2ogrgeometry.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml_read.py

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

TEST(GmlTest, Empty) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("empty"));
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(""));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("empty"));
}

TEST(GmlTest, Junk) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("abc"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("recognized"));
}

TEST(GmlTest, InvalidXmlJunk) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<a></differs>"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("matching"));
}

TEST(GmlTest, ValidXmlJunk) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<a></a>"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("recognized"));
}

TEST(GmlTest, ValidXmlJunkWithNamespacePrefixs) {
  // Trigger the ':' present case in BareGMLElement().
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:a></gml:a>"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("recognized"));
}

TEST(GmlTest, XmlHeaderAndValidXmlJunk) {
WithQuietHandler error_handler;  EXPECT_EQ(nullptr,
            OGR_G_CreateFromGML(
                R"xml(<?xml version="1.0" encoding="utf-8" ?><a></a>"))xml"));
}

TEST(GmlTest, ValidXmlComment) {
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<!-- comments are skipped -->"));
}

TEST(GmlTest, ValidPoint) {
  constexpr char kGml[] =
      "<gml:Point><gml:coordinates>-0.9,0.8,0.7</gml:coordinates></gml:Point>";
  OGRGeometryH geom_handle = OGR_G_CreateFromGML(kGml);
  ASSERT_NE(nullptr, geom_handle);
  auto geom = absl::WrapUnique(static_cast<OGRGeometry *>(geom_handle));
  EXPECT_EQ(wkbPoint25D, geom->getGeometryType());
}

TEST(GmlTest, InvalidPointCoordinate) {
  WithQuietHandler error_handler;
  constexpr char kGml[] =
      "<gml:Point><gml:coordinates>1</gml:coordinates></gml:Point>";
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Corrupt"));
}

TEST(GmlTest, ArcByBulge) {
  constexpr char kGml[] =
      "<gml:ArcByBulge><gml:posList>2 0 -2 0</gml:posList>"
      "<gml:bulge>2</gml:bulge><gml:normal>-1</gml:normal></gml:ArcByBulge>";
  OGRGeometryH geom_handle = OGR_G_CreateFromGML(kGml);
  ASSERT_NE(nullptr, geom_handle);
  auto geom = absl::WrapUnique(static_cast<OGRGeometry *>(geom_handle));
  EXPECT_EQ(wkbCircularString, geom->getGeometryType());
}

TEST(GmlTest, ArcByBulgeInvalid) {
  WithQuietHandler error_handler;
  // Bulge is missing a child.
  constexpr char kGml[] = "<ArcByBulge><bulge/></ArcByBulge>";
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("bulge element"));
}

TEST(GmlTest, PointsComprehensive) {
  // Simple 2D point pos
  {
    constexpr char kGml[] = "<gml:Point><gml:pos>1.5 2.5</gml:pos></gml:Point>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
    EXPECT_DOUBLE_EQ(1.5, static_cast<OGRPoint*>(geom.get())->getX());
    EXPECT_DOUBLE_EQ(2.5, static_cast<OGRPoint*>(geom.get())->getY());
  }

  // 3D point pos with srsName and srsDimension
  {
    constexpr char kGml[] =
        "<gml:Point srsName=\"EPSG:4326\" srsDimension=\"3\">"
        "<gml:pos>1 2 3</gml:pos></gml:Point>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint25D, geom->getGeometryType());
    EXPECT_DOUBLE_EQ(3.0, static_cast<OGRPoint*>(geom.get())->getZ());
  }

  // coordinates with custom cs and ts
  {
    constexpr char kGml[] =
        "<gml:Point><gml:coordinates cs=\",\" ts=\" \">"
        "10.0,20.0</gml:coordinates></gml:Point>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
  }

  // coord element (X, Y, Z)
  {
    constexpr char kGml[] =
        "<gml:Point><gml:coord><gml:X>10</gml:X><gml:Y>20</gml:Y><gml:Z>30</"
        "gml:Z>"
        "</gml:coord></gml:Point>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint25D, geom->getGeometryType());
  }

  // Other point tags: ElevatedPoint, ConnectionPoint, PointType
  for (const char* tag : {"ElevatedPoint", "ConnectionPoint", "PointType"}) {
    std::string gml = std::string("<gml:") + tag +
                      "><gml:pos>1 2</gml:pos></gml:" + tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
  }

  // Invalid point cases
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:Point/>"));
    OGR_G_DestroyGeometry(
        OGR_G_CreateFromGML("<gml:Point><gml:pos/></gml:Point>"));
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(
                           "<gml:Point><gml:pos>1</gml:pos></gml:Point>"));
    EXPECT_EQ(
        nullptr,
        OGR_G_CreateFromGML(
            "<gml:Point><gml:coord><gml:X>1</gml:X></gml:coord></gml:Point>"));
    OGR_G_DestroyGeometry(
        OGR_G_CreateFromGML("<gml:Point><gml:coordinates/></gml:Point>"));
  }
}

TEST(GmlTest, BoxAndEnvelope) {
  // Box 2D
  {
    constexpr char kGml[] =
        "<gml:Box><gml:coord><gml:X>0</gml:X><gml:Y>1</gml:Y></gml:coord>"
        "<gml:coord><gml:X>10</gml:X><gml:Y>11</gml:Y></gml:coord></gml:Box>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }

  // Box 3D
  {
    constexpr char kGml[] =
        "<gml:Box srsDimension=\"3\">"
        "<gml:coord><gml:X>0</gml:X><gml:Y>1</gml:Y><gml:Z>2</gml:Z></"
        "gml:coord>"
        "<gml:coord><gml:X>10</gml:X><gml:Y>11</gml:Y><gml:Z>12</gml:Z></"
        "gml:coord></gml:Box>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon25D, geom->getGeometryType());
  }

  // BoxType with coordinates
  {
    constexpr char kGml[] =
        "<gml:BoxType><gml:coordinates>0,0 "
        "10,10</gml:coordinates></gml:BoxType>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }

  // Envelope with lowerCorner / upperCorner 2D and 3D
  {
    constexpr char kGml[] =
        "<gml:Envelope><gml:lowerCorner>0 1</gml:lowerCorner>"
        "<gml:upperCorner>10 11</gml:upperCorner></gml:Envelope>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:Envelope srsDimension=\"3\"><gml:lowerCorner>0 1 "
        "2</gml:lowerCorner>"
        "<gml:upperCorner>10 11 12</gml:upperCorner></gml:Envelope>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_TRUE(geom->getGeometryType() == wkbPolygon ||
                geom->getGeometryType() == wkbPolygon25D);
  }

  // Envelope with pos (invalid)
  {
    WithQuietHandler quiet;
    constexpr char kGml[] =
        "<gml:Envelope><gml:pos>0 1</gml:pos><gml:pos>10 "
        "11</gml:pos></gml:Envelope>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  }

  // Invalid Box / Envelope
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        nullptr,
        OGR_G_CreateFromGML(
            "<gml:Box><gml:coord><gml:X>0</gml:X></gml:coord></gml:Box>"));
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:Envelope/>"));
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:Envelope><gml:lowerCorner>0</"
                                           "gml:lowerCorner></gml:Envelope>"));
  }
}

TEST(GmlTest, LineStringComprehensive) {
  // posList 2D and 3D
  {
    constexpr char kGml[] =
        "<gml:LineString><gml:posList>0 0 1 1 2 "
        "0</gml:posList></gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    EXPECT_EQ(3, static_cast<OGRLineString*>(geom.get())->getNumPoints());
  }
  {
    constexpr char kGml[] =
        "<gml:LineString srsDimension=\"3\">"
        "<gml:posList>0 0 1 1 1 2 2 0 3</gml:posList></gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString25D, geom->getGeometryType());
  }

  // coordinates
  {
    constexpr char kGml[] =
        "<gml:LineString><gml:coordinates>0,0 1,1 "
        "2,0</gml:coordinates></gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // pointProperty / pointRep / pos / coord
  {
    constexpr char kGml[] =
        "<gml:LineString>"
        "<gml:pointProperty><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:pointProperty>"
        "<gml:pointRep><gml:Point><gml:pos>1 "
        "1</gml:pos></gml:Point></gml:pointRep>"
        "<gml:pos>2 0</gml:pos>"
        "<gml:coord><gml:X>3</gml:X><gml:Y>1</gml:Y></gml:coord>"
        "</gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    EXPECT_EQ(2, static_cast<OGRLineString*>(geom.get())->getNumPoints());
  }

  // LineStringSegment, Geodesic, GeodesicString
  for (const char* tag : {"LineStringSegment", "Geodesic", "GeodesicString"}) {
    std::string gml = std::string("<gml:") + tag +
                      "><gml:posList>0 0 1 1</gml:posList></gml:" + tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // Invalid line strings
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:LineString/>"));
    OGR_G_DestroyGeometry(OGR_G_CreateFromGML(
        "<gml:LineString><gml:posList>0 0</gml:posList></gml:LineString>"));
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:LineString><gml:posList>0 0 "
                                           "1</gml:posList></gml:LineString>"));
  }
}

TEST(GmlTest, ArcAndCircularString) {
  // Arc
  {
    constexpr char kGml[] =
        "<gml:Arc><gml:posList>0 0 1 1 2 0</gml:posList></gml:Arc>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
  }

  // ArcString
  {
    constexpr char kGml[] =
        "<gml:ArcString><gml:posList>0 0 1 1 2 0 3 -1 4 "
        "0</gml:posList></gml:ArcString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
    EXPECT_EQ(5, static_cast<OGRCircularString*>(geom.get())->getNumPoints());
  }

  // Circle
  {
    constexpr char kGml[] =
        "<gml:Circle><gml:posList>0 0 1 1 2 0</gml:posList></gml:Circle>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
  }

  // ArcByCenterPoint angles
  {
    constexpr char kGml[] =
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>0 0</gml:pos><gml:radius uom=\"m\">10</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">90</gml:endAngle>"
        "</gml:ArcByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
  }

  // ArcByCenterPoint with coordinates
  {
    WithQuietHandler quiet;
    constexpr char kGml[] =
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>0 0</gml:pos><gml:radius uom=\"m\">10</gml:radius>"
        "<gml:coordinates>10,0</gml:coordinates><gml:coordinates>0,10</"
        "gml:coordinates>"
        "</gml:ArcByCenterPoint>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  }

  // CircleByCenterPoint
  {
    constexpr char kGml[] =
        "<gml:CircleByCenterPoint>"
        "<gml:pos>0 0</gml:pos><gml:radius uom=\"m\">10</gml:radius>"
        "</gml:CircleByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
  }

  // Invalid Arc cases
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:Arc/>"));
    EXPECT_EQ(nullptr,
              OGR_G_CreateFromGML(
                  "<gml:Arc><gml:posList>0 0 1 1</gml:posList></gml:Arc>"));
    EXPECT_EQ(
        nullptr,
        OGR_G_CreateFromGML(
            "<gml:Circle><gml:posList>0 0 1 1</gml:posList></gml:Circle>"));
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:ArcByCenterPoint/>"));
  }
}

TEST(GmlTest, LinearRingAndRing) {
  // LinearRing with posList and coordinates
  {
    constexpr char kGml[] =
        "<gml:LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:LinearRing><gml:coordinates>0,0 0,1 1,1 1,0 "
        "0,0</gml:coordinates></gml:LinearRing>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // Ring with curveMember
  {
    constexpr char kGml[] =
        "<gml:Ring>"
        "<gml:curveMember><gml:LineString><gml:posList>0 0 0 "
        "1</gml:posList></gml:LineString></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>0 1 1 "
        "1</gml:posList></gml:LineString></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>1 1 1 "
        "0</gml:posList></gml:LineString></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>1 0 0 "
        "0</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:Ring>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // Ring with circular string curveMember
  {
    constexpr char kGml[] =
        "<gml:Ring>"
        "<gml:curveMember><gml:LineString><gml:posList>0 0 0 "
        "2</gml:posList></gml:LineString></gml:curveMember>"
        "<gml:curveMember><gml:Arc><gml:posList>0 2 1 3 2 "
        "2</gml:posList></gml:Arc></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>2 2 0 "
        "0</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:Ring>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCompoundCurve, geom->getGeometryType());
  }
}

TEST(GmlTest, PolygonsAndVariants) {
  // GML2 Polygon with outerBoundaryIs and innerBoundaryIs
  {
    constexpr char kGml[] =
        "<gml:Polygon>"
        "<gml:outerBoundaryIs><gml:LinearRing><gml:posList>0 0 0 10 10 10 10 0 "
        "0 0</gml:posList></gml:LinearRing></gml:outerBoundaryIs>"
        "<gml:innerBoundaryIs><gml:LinearRing><gml:posList>2 2 2 4 4 4 4 2 2 "
        "2</gml:posList></gml:LinearRing></gml:innerBoundaryIs>"
        "</gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
    EXPECT_EQ(2,
              static_cast<OGRPolygon*>(geom.get())->getNumInteriorRings() + 1);
  }

  // GML3 Polygon with exterior and interior
  {
    constexpr char kGml[] =
        "<gml:Polygon>"
        "<gml:exterior><gml:LinearRing><gml:posList>0 0 0 10 10 10 10 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior>"
        "<gml:interior><gml:LinearRing><gml:posList>2 2 2 4 4 4 4 2 2 "
        "2</gml:posList></gml:LinearRing></gml:interior>"
        "</gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }

  // Polygon with curved Ring (CurvePolygon)
  {
    constexpr char kGml[] =
        "<gml:Polygon>"
        "<gml:exterior>"
        "<gml:Ring>"
        "<gml:curveMember><gml:Arc><gml:posList>0 0 1 1 2 "
        "0</gml:posList></gml:Arc></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>2 0 0 "
        "0</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:Ring>"
        "</gml:exterior>"
        "</gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCurvePolygon, geom->getGeometryType());
  }

  // PolygonPatch, Rectangle, Triangle
  for (const char* tag : {"PolygonPatch", "Rectangle"}) {
    std::string gml =
        std::string("<gml:") + tag +
        "><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:" +
        tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }

  // Triangle
  {
    constexpr char kGml[] =
        "<gml:Triangle>"
        "<gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior>"
        "</gml:Triangle>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbTriangle, geom->getGeometryType());
  }

  // SimplePolygon, SimpleRectangle, SimpleTriangle
  for (const char* tag : {"SimplePolygon", "SimpleRectangle"}) {
    std::string gml =
        std::string("<gml:") + tag +
        "><gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList></gml:" + tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:SimpleTriangle><gml:posList>0 0 0 1 1 0 0 "
        "0</gml:posList></gml:SimpleTriangle>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbTriangle, geom->getGeometryType());
  }
}

TEST(GmlTest, MultiGeometries) {
  // MultiPoint with pointMember, pointMembers, posList, SimpleMultiPoint
  {
    constexpr char kGml[] =
        "<gml:MultiPoint>"
        "<gml:pointMember><gml:Point><gml:pos>1 "
        "2</gml:pos></gml:Point></gml:pointMember>"
        "<gml:pointMember><gml:Point><gml:pos>3 "
        "4</gml:pos></gml:Point></gml:pointMember>"
        "</gml:MultiPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
    EXPECT_EQ(2, static_cast<OGRMultiPoint*>(geom.get())->getNumGeometries());
  }
  {
    constexpr char kGml[] =
        "<gml:MultiPoint>"
        "<gml:pointMembers><gml:Point><gml:pos>1 "
        "2</gml:pos></gml:Point><gml:Point><gml:pos>3 "
        "4</gml:pos></gml:Point></gml:pointMembers>"
        "</gml:MultiPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:MultiPoint><gml:posList>1 2 3 4</gml:posList></gml:MultiPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:SimpleMultiPoint><gml:posList>1 2 3 "
        "4</gml:posList></gml:SimpleMultiPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
  }

  // MultiLineString
  {
    constexpr char kGml[] =
        "<gml:MultiLineString>"
        "<gml:lineStringMember><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:lineStringMember>"
        "</gml:MultiLineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiLineString, geom->getGeometryType());
  }

  // MultiCurve with curveMember and curveMembers
  {
    constexpr char kGml[] =
        "<gml:MultiCurve>"
        "<gml:curveMember><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveMember>"
        "<gml:curveMember><gml:Arc><gml:posList>1 1 2 2 3 "
        "1</gml:posList></gml:Arc></gml:curveMember>"
        "</gml:MultiCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiCurve, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:MultiCurve>"
        "<gml:curveMembers><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveMembers>"
        "</gml:MultiCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiLineString, geom->getGeometryType());
  }

  // MultiPolygon, MultiSurface, CompositeSurface, Shell
  for (const char* tag :
       {"MultiPolygon", "MultiSurface", "CompositeSurface", "Shell"}) {
    std::string gml = std::string("<gml:") + tag +
                      "><gml:surfaceMember><gml:Polygon><gml:outerBoundaryIs><"
                      "gml:LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 "
                      "0</gml:posList></gml:LinearRing></gml:outerBoundaryIs></"
                      "gml:Polygon></gml:surfaceMember></gml:" +
                      tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_TRUE(geom->getGeometryType() == wkbMultiPolygon ||
                geom->getGeometryType() == wkbMultiSurface);
  }

  // GeometryCollection and MultiGeometry
  for (const char* tag : {"GeometryCollection", "MultiGeometry"}) {
    std::string gml =
        std::string("<gml:") + tag +
        "><gml:geometryMember><gml:Point><gml:pos>1 "
        "2</gml:pos></gml:Point></gml:geometryMember>"
        "<gml:geometryMember><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:geometryMember></gml:" +
        tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbGeometryCollection, geom->getGeometryType());
  }
}

TEST(GmlTest, CurvesAndSurfaces) {
  // Curve and ElevatedCurve with segments
  for (const char* tag : {"Curve", "ElevatedCurve"}) {
    std::string gml = std::string("<gml:") + tag +
                      "><gml:segments><gml:LineStringSegment><gml:posList>0 0 "
                      "1 1</gml:posList></gml:LineStringSegment>"
                      "<gml:Arc><gml:posList>1 1 2 2 3 "
                      "1</gml:posList></gml:Arc></gml:segments></gml:" +
                      tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCompoundCurve, geom->getGeometryType());
  }

  // CompositeCurve
  {
    constexpr char kGml[] =
        "<gml:CompositeCurve>"
        "<gml:curveMember><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>1 1 2 "
        "2</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:CompositeCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // Surface and ElevatedSurface with patches
  for (const char* tag : {"Surface", "ElevatedSurface"}) {
    std::string gml = std::string("<gml:") + tag +
                      "><gml:patches><gml:PolygonPatch><gml:exterior><gml:"
                      "LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 "
                      "0</gml:posList></gml:LinearRing></gml:exterior></"
                      "gml:PolygonPatch></gml:patches></gml:" +
                      tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_TRUE(geom->getGeometryType() == wkbPolygon ||
                geom->getGeometryType() == wkbPolyhedralSurface);
  }

  // TriangulatedSurface and Tin
  for (const char* tag : {"TriangulatedSurface", "Tin"}) {
    std::string gml = std::string("<gml:") + tag +
                      "><gml:trianglePatches><gml:Triangle><gml:exterior><gml:"
                      "LinearRing><gml:posList>0 0 0 1 1 0 0 "
                      "0</gml:posList></gml:LinearRing></gml:exterior></"
                      "gml:Triangle></gml:trianglePatches></gml:" +
                      tag + ">";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << tag;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbTIN, geom->getGeometryType());
  }

  // PolyhedralSurface
  {
    constexpr char kGml[] =
        "<gml:PolyhedralSurface>"
        "<gml:polygonPatches><gml:PolygonPatch><gml:exterior><gml:LinearRing><"
        "gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch></"
        "gml:polygonPatches>"
        "</gml:PolyhedralSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolyhedralSurface, geom->getGeometryType());
  }

  // Solid
  {
    constexpr char kGml[] =
        "<gml:Solid>"
        "<gml:exterior><gml:CompositeSurface><gml:surfaceMember><gml:Polygon><"
        "gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon></"
        "gml:surfaceMember></gml:CompositeSurface></gml:exterior>"
        "</gml:Solid>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolyhedralSurface, geom->getGeometryType());
  }
}

TEST(GmlTest, OrientableGeometries) {
  // OrientableCurve + and -
  {
    constexpr char kGml[] =
        "<gml:OrientableCurve orientation=\"+\">"
        "<gml:baseCurve><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:baseCurve>"
        "</gml:OrientableCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:OrientableCurve "
        "orientation=\"-\"><gml:baseCurve><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:baseCurve></"
        "gml:OrientableCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    EXPECT_DOUBLE_EQ(1.0, static_cast<OGRLineString*>(geom.get())->getX(0));
  }

  // OrientableSurface + and -
  {
    constexpr char kGml[] =
        "<gml:OrientableSurface orientation=\"+\">"
        "<gml:baseSurface><gml:Polygon><gml:exterior><gml:LinearRing><gml:"
        "posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon></"
        "gml:baseSurface>"
        "</gml:OrientableSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:OrientableSurface orientation=\"-\">"
        "<gml:baseSurface><gml:Polygon><gml:exterior><gml:LinearRing><gml:"
        "posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon></"
        "gml:baseSurface>"
        "</gml:OrientableSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }
}

TEST(GmlTest, TopoElements) {
  // directedEdge + and -
  {
    constexpr char kGml[] =
        "<gml:directedEdge orientation=\"+\">"
        "<gml:Edge><gml:curveProperty><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveProperty></gml:Edge>"
        "</gml:directedEdge>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }
  {
    constexpr char kGml[] =
        "<gml:directedEdge orientation=\"-\">"
        "<gml:Edge><gml:curveProperty><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveProperty></gml:Edge>"
        "</gml:directedEdge>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    EXPECT_DOUBLE_EQ(1.0, static_cast<OGRLineString*>(geom.get())->getX(0));
  }

  // directedEdge with directedNode secondary geometry
  {
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", "YES");
    constexpr char kGml[] =
        "<gml:directedEdge orientation=\"+\">"
        "<gml:Edge>"
        "<gml:directedNode "
        "orientation=\"+\"><gml:Node><gml:pointProperty><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:pointProperty></gml:Node></"
        "gml:directedNode>"
        "<gml:directedNode "
        "orientation=\"-\"><gml:Node><gml:pointProperty><gml:Point><gml:pos>1 "
        "1</gml:pos></gml:Point></gml:pointProperty></gml:Node></"
        "gml:directedNode>"
        "<gml:curveProperty><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveProperty>"
        "</gml:Edge>"
        "</gml:directedEdge>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", nullptr);
  }

  // TopoCurve
  {
    constexpr char kGml[] =
        "<gml:TopoCurve>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>1 1 2 "
        "2</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:TopoCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiLineString, geom->getGeometryType());
  }

  // TopoSurface
  {
    constexpr char kGml[] =
        "<gml:TopoSurface>"
        "<gml:directedFace orientation=\"+\">"
        "<gml:Face>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:Face>"
        "</gml:directedFace>"
        "</gml:TopoSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }
}

TEST(GmlTest, XMLNodeDirectAndTrees) {
  // OGR_G_CreateFromGMLTree
  {
    EXPECT_EQ(nullptr, OGR_G_CreateFromGMLTree(nullptr));
    constexpr char kXml[] = "<gml:Point><gml:pos>1 2</gml:pos></gml:Point>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometryH hGeom = OGR_G_CreateFromGMLTree(psTree);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
    CPLDestroyXMLNode(psTree);
  }

  // GML2OGRGeometry_XMLNode direct options
  {
    constexpr char kXml[] =
        "<gml:Polygon gml:id=\"poly1\">"
        "<gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior>"
        "</gml:Polygon>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometry* poGeom =
        GML2OGRGeometry_XMLNode(psTree, 0, 0, 2, true, true, true, "poly1");
    ASSERT_NE(nullptr, poGeom);
    delete poGeom;
    CPLDestroyXMLNode(psTree);
  }

  // Deep recursion error
  {
    WithQuietHandler quiet;
    constexpr char kXml[] = "<gml:Point><gml:pos>1 2</gml:pos></gml:Point>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometry* poGeom = GML2OGRGeometry_XMLNode(psTree, 0, 32);
    EXPECT_EQ(nullptr, poGeom);
    CPLDestroyXMLNode(psTree);
  }
}

TEST(GmlTest, AdditionalCoverageBatch) {
  // TopoSurface with bFaceHoleNegative = true
  {
    constexpr char kXml[] =
        "<gml:TopoSurface>"
        "<gml:directedFace orientation=\"+\"><gml:Face>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:Face></gml:directedFace>"
        "<gml:directedFace orientation=\"-\"/>"
        "</gml:TopoSurface>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometry* poGeom =
        GML2OGRGeometry_XMLNode(psTree, 0, 0, 2, false, true, true, "topo1");
    ASSERT_NE(nullptr, poGeom);
    EXPECT_EQ(wkbPolygon, poGeom->getGeometryType());
    delete poGeom;
    CPLDestroyXMLNode(psTree);
  }

  // directedEdge with 2 directedNodes and Node under Edge
  {
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", "YES");
    constexpr char kGml[] =
        "<gml:directedEdge orientation=\"+\">"
        "<gml:Edge>"
        "<gml:directedNode "
        "orientation=\"+\"><gml:Node><gml:pointProperty><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:pointProperty></gml:Node></"
        "gml:directedNode>"
        "<gml:directedNode orientation=\"-\"/>"
        "<gml:Node><gml:pointProperty><gml:Point><gml:pos>1 "
        "1</gml:pos></gml:Point></gml:pointProperty></gml:Node>"
        "<gml:curveProperty><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveProperty>"
        "</gml:Edge>"
        "</gml:directedEdge>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", nullptr);
  }

  // ArcByCenterPoint with EPSG:4326 and various UOMs (km, nm, mi, ft)
  for (const char* uom : {"km", "nm", "mi", "ft"}) {
    std::string gml = std::string(
                          "<gml:ArcByCenterPoint numArc=\"1\" "
                          "srsName=\"urn:ogc:def:crs:EPSG::4326\">") +
                      "<gml:pos>40 10</gml:pos><gml:radius uom=\"" + uom +
                      "\">1</gml:radius>" +
                      "<gml:startAngle uom=\"deg\">0</gml:startAngle>" +
                      "<gml:endAngle uom=\"deg\">90</gml:endAngle>" +
                      "</gml:ArcByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(gml.c_str());
    ASSERT_NE(nullptr, hGeom) << uom;
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // Projected CRS ArcByCenterPoint
  {
    constexpr char kGml[] =
        "<gml:ArcByCenterPoint numArc=\"1\" srsName=\"EPSG:32631\">"
        "<gml:pos>500000 0</gml:pos><gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">90</gml:endAngle>"
        "</gml:ArcByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
  }

  // CircleByCenterPoint with geographic SRS
  {
    constexpr char kGml[] =
        "<gml:CircleByCenterPoint srsName=\"urn:ogc:def:crs:EPSG::4326\">"
        "<gml:pos>40 10</gml:pos><gml:radius uom=\"km\">1</gml:radius>"
        "</gml:CircleByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // Unhandled UOM warning
  {
    WithQuietHandler quiet;
    constexpr char kGml[] =
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>0 0</gml:pos><gml:radius uom=\"furlong\">1</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">90</gml:endAngle>"
        "</gml:ArcByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    if (hGeom) OGR_G_DestroyGeometry(hGeom);
  }

  // Surface with multiple PolygonPatch elements (creates MultiPolygon /
  // MultiSurface)
  {
    constexpr char kGml[] =
        "<gml:Surface>"
        "<gml:patches>"
        "<gml:PolygonPatch><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 "
        "1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>"
        "<gml:PolygonPatch><gml:exterior><gml:LinearRing><gml:posList>2 2 2 3 "
        "3 3 3 2 2 "
        "2</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>"
        "</gml:patches>"
        "</gml:Surface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }

  // Surface with multiple Triangle elements (creates TriangulatedSurface)
  {
    constexpr char kGml[] =
        "<gml:Surface>"
        "<gml:patches>"
        "<gml:Triangle><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 0 "
        "0 0</gml:posList></gml:LinearRing></gml:exterior></gml:Triangle>"
        "<gml:Triangle><gml:exterior><gml:LinearRing><gml:posList>1 0 0 1 1 1 "
        "1 0</gml:posList></gml:LinearRing></gml:exterior></gml:Triangle>"
        "</gml:patches>"
        "</gml:Surface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbTIN, geom->getGeometryType());
  }

  // GeometryCollection with geometryMembers (plural)
  {
    constexpr char kGml[] =
        "<gml:GeometryCollection>"
        "<gml:geometryMembers>"
        "<gml:Point><gml:pos>1 2</gml:pos></gml:Point>"
        "<gml:Point><gml:pos>3 4</gml:pos></gml:Point>"
        "</gml:geometryMembers>"
        "</gml:GeometryCollection>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbGeometryCollection, geom->getGeometryType());
    EXPECT_EQ(
        2, static_cast<OGRGeometryCollection*>(geom.get())->getNumGeometries());
  }

  // CompositeCurve with curveMembers (plural)
  {
    constexpr char kGml[] =
        "<gml:CompositeCurve>"
        "<gml:curveMembers>"
        "<gml:LineString><gml:posList>0 0 1 1</gml:posList></gml:LineString>"
        "<gml:LineString><gml:posList>1 1 2 2</gml:posList></gml:LineString>"
        "</gml:curveMembers>"
        "</gml:CompositeCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // MultiSurface with surfaceMembers (plural)
  {
    constexpr char kGml[] =
        "<gml:MultiSurface>"
        "<gml:surfaceMembers>"
        "<gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:posList>0 0 0 "
        "1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon>"
        "<gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:posList>2 2 2 "
        "3 3 3 3 2 2 "
        "2</gml:posList></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon>"
        "</gml:surfaceMembers>"
        "</gml:MultiSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }

  // MultiSurface with nested MultiPolygon in surfaceMember
  {
    constexpr char kGml[] =
        "<gml:MultiSurface>"
        "<gml:surfaceMember>"
        "<gml:MultiPolygon>"
        "<gml:polygonMember><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><"
        "gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon></"
        "gml:polygonMember>"
        "</gml:MultiPolygon>"
        "</gml:surfaceMember>"
        "</gml:MultiSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }

  // MultiSurface reconstruct topology (patches with interior rings)
  {
    constexpr char kGml[] =
        "<gml:MultiSurface>"
        "<gml:surfaceMember>"
        "<gml:Surface>"
        "<gml:patches>"
        "<gml:PolygonPatch>"
        "<gml:interior><gml:LinearRing><gml:posList>0 0 0 10 10 10 10 0 0 "
        "0</gml:posList></gml:LinearRing></gml:interior>"
        "<gml:interior><gml:LinearRing><gml:posList>2 2 2 4 4 4 4 2 2 "
        "2</gml:posList></gml:LinearRing></gml:interior>"
        "</gml:PolygonPatch>"
        "</gml:patches>"
        "</gml:Surface>"
        "</gml:surfaceMember>"
        "</gml:MultiSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    if (hGeom) OGR_G_DestroyGeometry(hGeom);
  }

  // Polygon with interior LineString (promoted to LinearRing)
  {
    constexpr char kGml[] =
        "<gml:Polygon>"
        "<gml:exterior><gml:LinearRing><gml:posList>0 0 0 10 10 10 10 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior>"
        "<gml:interior><gml:LineString><gml:posList>2 2 2 4 4 4 4 2 2 "
        "2</gml:posList></gml:LineString></gml:interior>"
        "</gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }

  // Polygon with interior curved Ring (promoted to CurvePolygon)
  {
    constexpr char kGml[] =
        "<gml:Polygon>"
        "<gml:exterior><gml:LinearRing><gml:posList>0 0 0 10 10 10 10 0 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior>"
        "<gml:interior>"
        "<gml:Ring>"
        "<gml:curveMember><gml:Arc><gml:posList>2 2 3 3 4 "
        "2</gml:posList></gml:Arc></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>4 2 2 "
        "2</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:Ring>"
        "</gml:interior>"
        "</gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCurvePolygon, geom->getGeometryType());
  }

  // Ring with ArcByCenterPoint segment connected to LineString
  {
    constexpr char kGml[] =
        "<gml:Polygon srsName=\"urn:ogc:def:crs:EPSG::4326\">"
        "<gml:exterior>"
        "<gml:Ring>"
        "<gml:curveMember>"
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>40 10</gml:pos><gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">180</gml:endAngle>"
        "</gml:ArcByCenterPoint>"
        "</gml:curveMember>"
        "<gml:curveMember>"
        "<gml:LineString><gml:posList>40 9.988 40 "
        "10.012</gml:posList></gml:LineString>"
        "</gml:curveMember>"
        "</gml:Ring>"
        "</gml:exterior>"
        "</gml:Polygon>";
    WithQuietHandler quiet;
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    if (hGeom) OGR_G_DestroyGeometry(hGeom);
  }

  // MultiLineString with invalid member (error path)
  {
    WithQuietHandler quiet;
    constexpr char kGml[] =
        "<gml:MultiLineString>"
        "<gml:lineStringMember><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:lineStringMember>"
        "</gml:MultiLineString>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  }

  // MultiSurface with invalid member (error path)
  {
    WithQuietHandler quiet;
    constexpr char kGml[] =
        "<gml:MultiSurface>"
        "<gml:surfaceMember><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:surfaceMember>"
        "</gml:MultiSurface>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  }
}

TEST(GmlTest, AdditionalHighCoverageBatch3) {
  // TopoSurface with 2 directedFaces touching/merging and disjoint (triggers
  // Union in lines 3170-3196)
  {
    constexpr char kGml[] =
        "<gml:TopoSurface>"
        "<gml:directedFace orientation=\"+\"><gml:Face>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:Face></gml:directedFace>"
        "<gml:directedFace orientation=\"+\"><gml:Face>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>2 2 2 3 3 3 3 2 2 "
        "2</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:Face></gml:directedFace>"
        "</gml:TopoSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }

  // TopoSurface with bFaceHoleNegative = true and negative face orientation
  // (lines 3270-3305)
  {
    constexpr char kXml[] =
        "<gml:TopoSurface>"
        "<gml:directedFace orientation=\"+\"><gml:Face>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>0 0 0 10 10 10 10 0 0 "
        "0</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:Face></gml:directedFace>"
        "<gml:directedFace orientation=\"-\">"
        "<gml:Face>"
        "<gml:directedEdge "
        "orientation=\"+\"><gml:Edge><gml:curveProperty><gml:LineString><gml:"
        "posList>2 2 2 4 4 4 4 2 2 "
        "2</gml:posList></gml:LineString></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>"
        "</gml:Face>"
        "</gml:directedFace>"
        "</gml:TopoSurface>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometry* poGeom =
        GML2OGRGeometry_XMLNode(psTree, 0, 0, 2, false, true, true, "topo_neg");
    ASSERT_NE(nullptr, poGeom);
    EXPECT_EQ(wkbPolygon, poGeom->getGeometryType());
    delete poGeom;
    CPLDestroyXMLNode(psTree);
  }

  // directedEdge exact tags for secondary geometry (lines 2864-2912)
  {
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", "YES");
    constexpr char kXml[] =
        "<directedEdge orientation=\"+\">"
        "<Edge>"
        "<directedNode orientation=\"+\"><Node><pointProperty><Point><pos>0 "
        "0</pos></Point></pointProperty></Node></directedNode>"
        "<directedNode orientation=\"-\"/>"
        "<Node><pointProperty><Point><pos>1 "
        "1</pos></Point></pointProperty></Node>"
        "<curveProperty><LineString><posList>0 0 1 "
        "1</posList></LineString></curveProperty>"
        "</Edge>"
        "</directedEdge>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometry* poGeom = GML2OGRGeometry_XMLNode(psTree, 1);
    ASSERT_NE(nullptr, poGeom);
    EXPECT_EQ(wkbMultiPoint, poGeom->getGeometryType());
    delete poGeom;
    CPLDestroyXMLNode(psTree);
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", nullptr);
  }

  // TopoCurve with secondary geometry (lines 3013-3027)
  {
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", "YES");
    constexpr char kXml[] =
        "<TopoCurve>"
        "<directedEdge orientation=\"+\">"
        "<Edge>"
        "<directedNode orientation=\"+\"><Node><pointProperty><Point><pos>0 "
        "0</pos></Point></pointProperty></Node></directedNode>"
        "<directedNode orientation=\"-\"/>"
        "<Node><pointProperty><Point><pos>1 "
        "1</pos></Point></pointProperty></Node>"
        "<curveProperty><LineString><posList>0 0 1 "
        "1</posList></LineString></curveProperty>"
        "</Edge>"
        "</directedEdge>"
        "</TopoCurve>";
    CPLXMLNode* psTree = CPLParseXMLString(kXml);
    ASSERT_NE(nullptr, psTree);
    OGRGeometry* poGeom = GML2OGRGeometry_XMLNode(psTree, 1);
    ASSERT_NE(nullptr, poGeom);
    EXPECT_EQ(wkbMultiPoint, poGeom->getGeometryType());
    delete poGeom;
    CPLDestroyXMLNode(psTree);
    CPLSetConfigOption("GML_GET_SECONDARY_GEOM", nullptr);
  }

  // Ring with ArcByCenterPoint inside Curve/segments (lines 1203-1324 and
  // 1534-1590)
  {
    constexpr char kGml[] =
        "<gml:Polygon srsName=\"urn:ogc:def:crs:EPSG::4326\">"
        "<gml:exterior>"
        "<gml:Ring>"
        "<gml:curveMember>"
        "<gml:Curve>"
        "<gml:segments>"
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>40 10</gml:pos><gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">180</gml:endAngle>"
        "</gml:ArcByCenterPoint>"
        "</gml:segments>"
        "</gml:Curve>"
        "</gml:curveMember>"
        "<gml:curveMember>"
        "<gml:LineString><gml:posList>40.008993 10 40 "
        "10</gml:posList></gml:LineString>"
        "</gml:curveMember>"
        "</gml:Ring>"
        "</gml:exterior>"
        "</gml:Polygon>";
    WithQuietHandler quiet;
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    if (hGeom) OGR_G_DestroyGeometry(hGeom);
  }

  // Ring with ArcByCenterPoint as the last curve (lines 1490-1520 and
  // 1559-1584)
  {
    constexpr char kGml[] =
        "<gml:Polygon srsName=\"urn:ogc:def:crs:EPSG::4326\">"
        "<gml:exterior>"
        "<gml:Ring>"
        "<gml:curveMember>"
        "<gml:LineString><gml:posList>40 10 40.008993 "
        "10</gml:posList></gml:LineString>"
        "</gml:curveMember>"
        "<gml:curveMember>"
        "<gml:Curve>"
        "<gml:segments>"
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>40 10</gml:pos><gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">180</gml:endAngle>"
        "</gml:ArcByCenterPoint>"
        "</gml:segments>"
        "</gml:Curve>"
        "</gml:curveMember>"
        "</gml:Ring>"
        "</gml:exterior>"
        "</gml:Polygon>";
    WithQuietHandler quiet;
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGml);
    if (hGeom) OGR_G_DestroyGeometry(hGeom);
  }

  // PolyhedralSurface empty and curved patch (lines 3530-3534, 3586-3592)
  {
    constexpr char kEmpty[] = "<gml:PolyhedralSurface/>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kEmpty);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolyhedralSurface, geom->getGeometryType());
    EXPECT_TRUE(geom->IsEmpty());
  }
  {
    WithQuietHandler quiet;
    constexpr char kInvalid[] =
        "<gml:PolyhedralSurface><gml:invalid/></gml:PolyhedralSurface>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kInvalid));
  }
  {
    constexpr char kCurved[] =
        "<gml:PolyhedralSurface><gml:polygonPatches><gml:PolygonPatch><gml:"
        "exterior><gml:Ring>"
        "<gml:curveMember><gml:Arc><gml:posList>0 0 1 1 2 "
        "0</gml:posList></gml:Arc></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>2 0 0 "
        "0</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:Ring></gml:exterior></gml:PolygonPatch></gml:polygonPatches></"
        "gml:PolyhedralSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kCurved);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolyhedralSurface, geom->getGeometryType());
  }

  // Empty Solid, TriangulatedSurface, Tin
  {
    constexpr char kSolid[] = "<gml:Solid/>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kSolid);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
      EXPECT_TRUE(geom->IsEmpty());
    }

    constexpr char kTri[] = "<gml:TriangulatedSurface/>";
    hGeom = OGR_G_CreateFromGML(kTri);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
      EXPECT_TRUE(geom->IsEmpty());
    }
  }
}

TEST(GML2OGRGeometryTest, ExtendedCoverage) {
  // 1. gml:id error reporting (ReportError with pszId)
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:LineString gml:id=\"ls1\" srsDimension=\"4\">"
        "<gml:posList>0 0 0 0 1 1 1 1</gml:posList></gml:LineString>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 2. Aixm / custom coordinate separators: cs=" " ts=","
  {
    constexpr char kGML[] =
        "<gml:LineString><gml:coordinates cs=\" \" ts=\",\">0 0,10 0,10 10,0 "
        "0</gml:coordinates></gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    EXPECT_EQ(4, geom->toLineString()->getNumPoints());
  }

  // 3. CompositeCurve nested within CompositeCurve
  {
    constexpr char kGML[] =
        "<gml:CompositeCurve><gml:curveMember><gml:CompositeCurve><gml:"
        "curveMember>"
        "<gml:LineString><gml:posList>0 0 1 1</gml:posList></gml:LineString>"
        "</gml:curveMember></gml:CompositeCurve></gml:curveMember></"
        "gml:CompositeCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // 4. Polygon with exterior as LineString (converted to LinearRing)
  {
    constexpr char kGML[] =
        "<gml:Polygon><gml:exterior><gml:LineString>"
        "<gml:posList>0 0 0 1 1 1 0 0</gml:posList>"
        "</gml:LineString></gml:exterior></gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  }

  // 5. Polygon with unclosed LineString exterior fails
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:Polygon><gml:exterior><gml:LineString>"
        "<gml:posList>0 0 0 1 1 1</gml:posList>"
        "</gml:LineString></gml:exterior></gml:Polygon>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 6. Polygon with Ring exterior and LinearRing interior (converted to
  // LineString)
  {
    constexpr char kGML[] =
        "<gml:Polygon><gml:exterior><gml:Ring>"
        "<gml:curveMember><gml:Arc><gml:posList>0 0 5 2 10 "
        "0</gml:posList></gml:Arc></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>10 0 10 10 0 10 0 "
        "0</gml:posList></gml:LineString></gml:curveMember>"
        "</gml:Ring></gml:exterior><gml:interior>"
        "<gml:LinearRing><gml:posList>1 1 1 2 2 2 1 "
        "1</gml:posList></gml:LinearRing>"
        "</gml:interior></gml:Polygon>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCurvePolygon, geom->getGeometryType());
  }

  // 7. Ring with single ArcByCenterPoint curveMember that closes
  {
    constexpr char kGML[] =
        "<gml:Ring "
        "srsName=\"urn:ogc:def:crs:EPSG::4326\"><gml:curveMember><gml:Curve><"
        "gml:segments>"
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>40.0 10.0</gml:pos>"
        "<gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">359.999</gml:endAngle>"
        "</gml:ArcByCenterPoint>"
        "</gml:segments></gml:Curve></gml:curveMember></gml:Ring>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // 8. Ring with single ArcByCenterPoint that is non-closed (error)
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:Ring "
        "srsName=\"urn:ogc:def:crs:EPSG::4326\"><gml:curveMember><gml:Curve><"
        "gml:segments>"
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>40.0 10.0</gml:pos>"
        "<gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">90</gml:endAngle>"
        "</gml:ArcByCenterPoint>"
        "</gml:segments></gml:Curve></gml:curveMember></gml:Ring>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 9. Ring with curveMember containing xlink:href (warning & returns null)
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:Ring><gml:curveMember xlink:href=\"#unresolved\"/></gml:Ring>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 10. Ring with curveMember containing MultiLineString
  {
    constexpr char kGML[] =
        "<gml:Ring><gml:curveMember><gml:MultiLineString>"
        "<gml:lineStringMember><gml:LineString><gml:posList>0 0 1 "
        "0</gml:posList></gml:LineString></gml:lineStringMember>"
        "<gml:lineStringMember><gml:LineString><gml:posList>1 0 1 1 0 "
        "0</gml:posList></gml:LineString></gml:lineStringMember>"
        "</gml:MultiLineString></gml:curveMember></gml:Ring>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // 11. MultiSurface with surfaceMembers containing PolygonPatch and Shell
  {
    constexpr char kGML[] =
        "<gml:MultiSurface><gml:surfaceMembers>"
        "<gml:PolygonPatch><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 "
        "1 1 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>"
        "<gml:Shell><gml:surfaceMember><gml:Polygon><gml:exterior><gml:"
        "LinearRing><gml:posList>2 0 2 1 3 1 2 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon></"
        "gml:surfaceMember></gml:Shell>"
        "</gml:surfaceMembers></gml:MultiSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }

  // 12. Surface with two Polygons followed by non-Polygon patch (converts
  // MultiPolygon to MultiSurface)
  {
    constexpr char kGML[] =
        "<gml:Surface><gml:patches>"
        "<gml:PolygonPatch><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 "
        "1 1 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>"
        "<gml:PolygonPatch><gml:exterior><gml:LinearRing><gml:posList>2 0 2 1 "
        "3 1 2 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>"
        "<gml:PolygonPatch><gml:exterior><gml:Ring><gml:curveMember><gml:Arc><"
        "gml:posList>4 0 5 1 6 0</gml:posList></gml:Arc></gml:curveMember>"
        "<gml:curveMember><gml:LineString><gml:posList>6 0 4 "
        "0</gml:posList></gml:LineString></gml:curveMember></gml:Ring></"
        "gml:exterior></gml:PolygonPatch>"
        "</gml:patches></gml:Surface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiSurface, geom->getGeometryType());
  }

  // 13. Surface with both PolygonPatch and Triangle (produces
  // GeometryCollection)
  {
    constexpr char kGML[] =
        "<gml:Surface><gml:patches>"
        "<gml:PolygonPatch><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 "
        "1 1 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>"
        "<gml:Triangle><gml:exterior><gml:LinearRing><gml:posList>2 0 2 1 3 0 "
        "2 0</gml:posList></gml:LinearRing></gml:exterior></gml:Triangle>"
        "</gml:patches></gml:Surface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbGeometryCollection, geom->getGeometryType());
  }

  // 14. Solid with <interior> and <exterior>
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:Solid><gml:interior/"
        "><gml:exterior><gml:CompositeSurface><gml:surfaceMember>"
        "<gml:Polygon><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 1 0 "
        "0</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon>"
        "</gml:surfaceMember></gml:CompositeSurface></gml:exterior></"
        "gml:Solid>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPolyhedralSurface, geom->getGeometryType());
  }

  // 15. Envelope with deficient coordinates returns null
  {
    constexpr char kGML[] =
        "<gml:Envelope><gml:lowerCorner>1</gml:lowerCorner><gml:upperCorner>2 "
        "3</gml:upperCorner></gml:Envelope>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 16. CircleByCenterPoint with projected SRS (lines 1984-1994)
  {
    constexpr char kGML[] =
        "<gml:CircleByCenterPoint srsName=\"EPSG:32632\"><gml:pos>500000 "
        "4000000</gml:pos>"
        "<gml:radius uom=\"m\">100</gml:radius></gml:CircleByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCircularString, geom->getGeometryType());
  }

  // 17. ArcByCenterPoint with non-inverted axis order (CRS:84 geographic)
  {
    constexpr char kGML[] =
        "<gml:Curve srsName=\"urn:ogc:def:crs:OGC:1.3:CRS84\"><gml:segments>"
        "<gml:ArcByCenterPoint numArc=\"1\">"
        "<gml:pos>10.0 40.0</gml:pos>"
        "<gml:radius uom=\"m\">1000</gml:radius>"
        "<gml:startAngle uom=\"deg\">0</gml:startAngle>"
        "<gml:endAngle uom=\"deg\">90</gml:endAngle>"
        "</gml:ArcByCenterPoint></gml:segments></gml:Curve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // 18. TopoSurface with consecutive directedEdges sharing vertices
  {
    constexpr char kGML[] =
        "<gml:TopoSurface><gml:directedFace><gml:Face>"
        "<gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "<gml:LineString><gml:posList>0 0 0 1</gml:posList></gml:LineString>"
        "</gml:curveProperty></gml:Edge></gml:directedEdge>"
        "<gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "<gml:LineString><gml:posList>0 1 1 1</gml:posList></gml:LineString>"
        "</gml:curveProperty></gml:Edge></gml:directedEdge>"
        "<gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "<gml:LineString><gml:posList>1 1 1 0</gml:posList></gml:LineString>"
        "</gml:curveProperty></gml:Edge></gml:directedEdge>"
        "<gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "<gml:LineString><gml:posList>1 0 0 0</gml:posList></gml:LineString>"
        "</gml:curveProperty></gml:Edge></gml:directedEdge>"
        "</gml:Face></gml:directedFace></gml:TopoSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_TRUE(geom->getGeometryType() == wkbPolygon ||
                geom->getGeometryType() == wkbMultiPolygon);
  }

  // 19. GML_FACE_HOLE_NEGATIVE=YES with exterior and hole faces
  {
    CPLSetConfigOption("GML_FACE_HOLE_NEGATIVE", "YES");
    constexpr char kGML[] =
        "<gml:TopoSurface>"
        "  <gml:directedFace orientation=\"+\"><gml:Face>"
        "    <gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>0 0 0 "
        "10</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "    <gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>0 10 10 "
        "10</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "    <gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>10 10 10 "
        "0</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "    <gml:directedEdge orientation=\"+\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>10 0 0 "
        "0</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "  </gml:Face></gml:directedFace>"
        "  <gml:directedFace orientation=\"-\"><gml:Face>"
        "    <gml:directedEdge orientation=\"-\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>1 1 1 "
        "2</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "    <gml:directedEdge orientation=\"-\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>1 2 2 "
        "2</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "    <gml:directedEdge orientation=\"-\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>2 2 2 "
        "1</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "    <gml:directedEdge orientation=\"-\"><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>2 1 1 "
        "1</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "  </gml:Face></gml:directedFace>"
        "</gml:TopoSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    CPLSetConfigOption("GML_FACE_HOLE_NEGATIVE", nullptr);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_TRUE(geom->getGeometryType() == wkbPolygon ||
                geom->getGeometryType() == wkbMultiPolygon);
  }

  // 20. TopoSurface with a hole (calls GML2FaceExtRing)
  {
    constexpr char kGML[] =
        "<gml:TopoSurface><gml:directedFace><gml:Face>"
        "<gml:directedEdge><gml:Edge><gml:curveProperty>"
        "<gml:LineString><gml:posList>0 0 0 10 10 10 10 0 0 "
        "0</gml:posList></gml:LineString>"
        "</gml:curveProperty></gml:Edge></gml:directedEdge>"
        "<gml:directedEdge><gml:Edge><gml:curveProperty>"
        "<gml:LineString><gml:posList>2 2 2 5 5 5 5 2 2 "
        "2</gml:posList></gml:LineString>"
        "</gml:curveProperty></gml:Edge></gml:directedEdge>"
        "</gml:Face></gml:directedFace></gml:TopoSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
      EXPECT_TRUE(geom->getGeometryType() == wkbPolygon ||
                  geom->getGeometryType() == wkbMultiPolygon);
    }
  }

  // 21. TopoSurface multiple adjacent faces unioning to a single Polygon
  {
    constexpr char kGML[] =
        "<gml:TopoSurface>"
        "  <gml:directedFace><gml:Face>"
        "    <gml:directedEdge><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>0 0 0 1 1 1 1 0 0 "
        "0</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "  </gml:Face></gml:directedFace>"
        "  <gml:directedFace><gml:Face>"
        "    <gml:directedEdge><gml:Edge><gml:curveProperty>"
        "      <gml:LineString><gml:posList>1 0 1 1 2 1 2 0 1 "
        "0</gml:posList></gml:LineString>"
        "    </gml:curveProperty></gml:Edge></gml:directedEdge>"
        "  </gml:Face></gml:directedFace>"
        "</gml:TopoSurface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }

  // 22. Ring where last child is ArcByCenterPoint that closes the ring
  {
    constexpr char kGML[] =
        "<gml:Ring srsName=\"urn:ogc:def:crs:EPSG::4326\">"
        "  <gml:curveMember>"
        "    <gml:LineString><gml:posList>40.0089932 10.0 39.9910068 "
        "10.0</gml:posList></gml:LineString>"
        "  </gml:curveMember>"
        "  <gml:curveMember>"
        "    <gml:Curve><gml:segments>"
        "      <gml:ArcByCenterPoint numArc=\"1\">"
        "        <gml:pos>40.0 10.0</gml:pos>"
        "        <gml:radius uom=\"m\">1000</gml:radius>"
        "        <gml:startAngle uom=\"deg\">180</gml:startAngle>"
        "        <gml:endAngle uom=\"deg\">359.999</gml:endAngle>"
        "      </gml:ArcByCenterPoint>"
        "    </gml:segments></gml:Curve>"
        "  </gml:curveMember>"
        "</gml:Ring>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
  }

  // 23. CompositeCurve nested within CompositeCurve with curved Arc
  {
    constexpr char kGML[] =
        "<gml:CompositeCurve><gml:curveMember><gml:CompositeCurve><gml:"
        "curveMember>"
        "<gml:Arc><gml:posList>0 0 1 1 2 0</gml:posList></gml:Arc>"
        "</gml:curveMember></gml:CompositeCurve></gml:curveMember></"
        "gml:CompositeCurve>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbCompoundCurve, geom->getGeometryType());
  }

  // 24. ReportFailure with pszId
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:Polygon "
        "gml:id=\"poly1\"><gml:exterior><gml:invalid/></gml:exterior></"
        "gml:Polygon>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 25. Triangle with LineString exterior
  {
    constexpr char kGML[] =
        "<gml:Triangle><gml:exterior><gml:LineString>"
        "<gml:posList>0 0 1 0 0 1 0 0</gml:posList>"
        "</gml:LineString></gml:exterior></gml:Triangle>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbTriangle, geom->getGeometryType());
  }

  // 26. Solid with invalid exterior
  {
    WithQuietHandler quiet;
    constexpr char kGML[] =
        "<gml:Solid><gml:exterior><gml:invalid/></gml:exterior></gml:Solid>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }
}

TEST(GML2OGRGeometryTest, MoreCoverageAndEdgeCases) {
  WithQuietHandler quiet;

  // 1. coordinates with invalid decimal attribute
  {
    constexpr char kGML[] =
        "<gml:Point><gml:coordinates "
        "decimal=\"invalid\">0,0</gml:coordinates></gml:Point>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 2. coordinates with custom decimal attribute
  {
    constexpr char kGML[] =
        "<gml:Point><gml:coordinates "
        "decimal=\"#\">0#5,1#5</gml:coordinates></gml:Point>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
    EXPECT_DOUBLE_EQ(0.5, geom->toPoint()->getX());
    EXPECT_DOUBLE_EQ(1.5, geom->toPoint()->getY());
  }

  // 3. pos with xlink:href
  {
    constexpr char kGML[] =
        "<gml:LineString><gml:pos xlink:href=\"#p1\"/><gml:pos>0 "
        "0</gml:pos><gml:pos>1 1</gml:pos></gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
      EXPECT_EQ(wkbLineString, geom->getGeometryType());
    }
  }

  // 4. posList empty
  {
    constexpr char kGML[] = "<gml:LineString><gml:posList/></gml:LineString>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
      EXPECT_TRUE(geom->IsEmpty());
    }
  }

  // 5. CompositeCurve with non-curve member
  {
    constexpr char kGML[] =
        "<gml:CompositeCurve><gml:curveMember><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:curveMember></gml:CompositeCurve>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 6. MultiSurface with invalid surfaceMember
  {
    constexpr char kGML[] =
        "<gml:MultiSurface><gml:surfaceMember><gml:invalid/></"
        "gml:surfaceMember></gml:MultiSurface>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 7. CircleByCenterPoint with unknown radius uom
  {
    constexpr char kGML[] =
        "<gml:CircleByCenterPoint gml:id=\"c1\"><gml:pos>0 "
        "0</gml:pos><gml:radius "
        "uom=\"lightyear\">1</gml:radius></gml:CircleByCenterPoint>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    ASSERT_NE(nullptr, hGeom);
    auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
  }

  // 8. Polygon with invalid interior ring
  {
    constexpr char kGML[] =
        "<gml:Polygon><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 1 0 "
        "0</gml:posList></gml:LinearRing></"
        "gml:exterior><gml:interior><gml:invalid/></gml:interior></"
        "gml:Polygon>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 9. Empty Triangle
  {
    constexpr char kGML[] = "<gml:Triangle><gml:exterior/></gml:Triangle>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
      EXPECT_EQ(wkbTriangle, geom->getGeometryType());
      EXPECT_TRUE(geom->IsEmpty());
    }
  }

  // 10. Triangle with invalid exterior
  {
    constexpr char kGML[] =
        "<gml:Triangle><gml:exterior><gml:invalid/></gml:exterior></"
        "gml:Triangle>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 11. Ring with non-closed curveMember
  {
    constexpr char kGML[] =
        "<gml:Ring><gml:curveMember><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:curveMember></gml:Ring>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 12. ArcString with bad number of points
  {
    constexpr char kGML[] =
        "<gml:ArcString><gml:posList>0 0 1 1</gml:posList></gml:ArcString>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 13. ArcByBulge missing normal
  {
    constexpr char kGML[] =
        "<gml:ArcByBulge><gml:posList>0 0 1 "
        "1</gml:posList><gml:bulge>1</gml:bulge></gml:ArcByBulge>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 14. ArcByBulge bad number of points
  {
    constexpr char kGML[] =
        "<gml:ArcByBulge><gml:posList>0 "
        "0</gml:posList><gml:bulge>1</gml:bulge><gml:normal>1</gml:normal></"
        "gml:ArcByBulge>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 15. ArcByCenterPoint missing endAngle
  {
    constexpr char kGML[] =
        "<gml:ArcByCenterPoint><gml:pos>0 "
        "0</gml:pos><gml:radius>1</gml:radius><gml:startAngle>0</"
        "gml:startAngle></gml:ArcByCenterPoint>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 16. CircleByCenterPoint missing radius
  {
    constexpr char kGML[] =
        "<gml:CircleByCenterPoint><gml:pos>0 "
        "0</gml:pos></gml:CircleByCenterPoint>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 17. Curve lacks segments
  {
    constexpr char kGML[] = "<gml:Curve></gml:Curve>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 18. segments with non-curve member
  {
    constexpr char kGML[] =
        "<gml:Curve><gml:segments><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:segments></gml:Curve>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 19. directedEdge missing Edge
  {
    constexpr char kGML[] =
        "<gml:directedEdge orientation=\"+\"></gml:directedEdge>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 20. directedEdge missing curveProperty
  {
    constexpr char kGML[] =
        "<gml:directedEdge><gml:Edge></gml:Edge></gml:directedEdge>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 21. directedEdge curveProperty not LineString
  {
    constexpr char kGML[] =
        "<gml:directedEdge><gml:Edge><gml:curveProperty><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:curveProperty></gml:Edge></"
        "gml:directedEdge>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 22. Surface missing patches
  {
    constexpr char kGML[] =
        "<gml:Surface><gml:patches></gml:patches></gml:Surface>";
    OGRGeometryH hGeom = OGR_G_CreateFromGML(kGML);
    if (hGeom) {
      auto geom = absl::WrapUnique(static_cast<OGRGeometry*>(hGeom));
    }
  }

  // 23. OrientableCurve missing baseCurve
  {
    constexpr char kGML[] = "<gml:OrientableCurve></gml:OrientableCurve>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 24. OrientableCurve baseCurve is not curve
  {
    constexpr char kGML[] =
        "<gml:OrientableCurve><gml:baseCurve><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:baseCurve></gml:OrientableCurve>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 25. OrientableSurface missing baseSurface
  {
    constexpr char kGML[] = "<gml:OrientableSurface></gml:OrientableSurface>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 26. MultiLineString with invalid member
  {
    constexpr char kGML[] =
        "<gml:MultiLineString><gml:lineStringMember><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:lineStringMember></gml:MultiLineString>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 27. MultiCurve with invalid member
  {
    constexpr char kGML[] =
        "<gml:MultiCurve><gml:curveMember><gml:Point><gml:pos>0 "
        "0</gml:pos></gml:Point></gml:curveMember></gml:MultiCurve>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }

  // 28. MultiPoint with invalid member
  {
    constexpr char kGML[] =
        "<gml:MultiPoint><gml:pointMember><gml:LineString><gml:posList>0 0 1 "
        "1</gml:posList></gml:LineString></gml:pointMember></gml:MultiPoint>";
    EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGML));
  }
}

}  // namespace
}  // namespace autotest2
