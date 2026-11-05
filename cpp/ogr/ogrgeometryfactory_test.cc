// Tests ogrgeometry.cpp.
//
// Copyright 2016 Google Inc. All Rights Reserved.
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
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
// Copyright (c) 2009-2012, Even Rouault <even dot rouault at mines-paris dot
// org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this library; if not, write to the
// Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

// See:
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_wkbwkt_geom.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_wktempty.py

#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using std::unique_ptr;

unique_ptr<OGRGeometry> GeomFromWkt(const char* wkt) {
  if (wkt == nullptr) return nullptr;
  OGRGeometry* geom = nullptr;
  unique_ptr<char, autotest2::FreeDeleter> wkt_copy(strdup(wkt));
  char* wkt_end = wkt_copy.get();
  EXPECT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&wkt_end, nullptr, &geom));
  // Updated to point to the just past last character consumed.
  EXPECT_EQ('\0', *wkt_end);

  return unique_ptr<OGRGeometry>(geom);
}

unique_ptr<OGRGeometry> GeomFromWkb(const unsigned char* wkb, int length) {
  if (wkb == nullptr) return nullptr;
  OGRGeometry* geom = nullptr;
  EXPECT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkb(const_cast<unsigned char*>(wkb),
                                              nullptr, &geom, length));

  return unique_ptr<OGRGeometry>(geom);
}

std::string Wkt(OGRGeometry& geom) {
  char* wkt_tmp = nullptr;
  EXPECT_EQ(OGRERR_NONE, geom.exportToWkt(&wkt_tmp));
  if (wkt_tmp == nullptr) return "";
  std::string wkt(wkt_tmp);
  CPLFree(wkt_tmp);

  return wkt;
}

TEST(OgrGeometryFactoryWkbWktTest, MultiPoint) {
  auto geom_from_wkt = GeomFromWkt(
      "MULTIPOINT ("
      "7.00121346837841 2.998531375167659,8.001213458890561 "
      "2.998531401259243,7.001213468029164 1.998531376986648)");
  EXPECT_EQ(0, geom_from_wkt->getDimension());
  EXPECT_EQ(wkbMultiPoint, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x01\x04\x00\x00\x00\x03\x00\x00\x00\x01\x01\x00\x00\x00\x00\x00\x7c"
      "\x1a\x3e\x01\x1c\x40\x00\x00\x83\x04\xfe\xfc\x07\x40\x01\x01\x00\x00"
      "\x00\x00\x80\xec\x0c\x9f\x00\x20\x40\x00\x80\x03\x08\xfe\xfc\x07\x40"
      "\x01\x01\x00\x00\x00\x00\x00\x76\x1a\x3e\x01\x1c\x40\x00\x00\x83\x09"
      "\xfc\xf9\xff\x3f";
  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  auto wkt_from_wkt = Wkt(*geom_from_wkt);
  auto wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGeometryFactoryWkbWktTest, MultiLineString) {
  auto geom_from_wkt = GeomFromWkt(
      "MULTILINESTRING ("
      "(5.001213487354107 2.998531323406496, "
      "5.001213487004861 1.998531325356453), "
      "(5.001213487004861 1.998531325356453, "
      "5.001213486772031 0.998531326316879), "
      "(3.001213506038766 1.998531273566186, "
      "5.001213487004861 1.998531325356453), "
      "(5.001213487004861 1.998531325356453, "
      "6.001213477458805 1.998531351098791))");
  EXPECT_EQ(wkbMultiLineString, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x01\x05\x00\x00\x00\x04\x00\x00\x00\x01\x02\x00\x00\x00\x02\x00\x00"
      "\x00\x00\x00\xc2\x1b\x3e\x01\x14\x40\x00\x80\x90\xfd\xfd\xfc\x07\x40"
      "\x00\x00\xbc\x1b\x3e\x01\x14\x40\x00\x00\xa7\xfb\xfb\xf9\xff\x3f\x01"
      "\x02\x00\x00\x00\x02\x00\x00\x00\x00\x00\xbc\x1b\x3e\x01\x14\x40\x00"
      "\x00\xa7\xfb\xfb\xf9\xff\x3f\x00\x00\xb8\x1b\x3e\x01\x14\x40\x00\x00"
      "\xd2\xf7\xf7\xf3\xef\x3f\x01\x02\x00\x00\x00\x02\x00\x00\x00\x00\x00"
      "\x06\x3a\x7c\x02\x08\x40\x00\x00\xc0\xed\xfb\xf9\xff\x3f\x00\x00\xbc"
      "\x1b\x3e\x01\x14\x40\x00\x00\xa7\xfb\xfb\xf9\xff\x3f\x01\x02\x00\x00"
      "\x00\x02\x00\x00\x00\x00\x00\xbc\x1b\x3e\x01\x14\x40\x00\x00\xa7\xfb"
      "\xfb\xf9\xff\x3f\x00\x00\x18\x1b\x3e\x01\x18\x40\x00\x00\x90\x02\xfc"
      "\xf9\xff\x3f";
  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const std::string wkt_from_wkt = Wkt(*geom_from_wkt);
  const std::string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGeometryFactoryWkbWktTest, GeometryCollection) {
  auto geom_from_wkt = GeomFromWkt(
      "GEOMETRYCOLLECTION("
      "POINT(5.0012134894 9.998531313699999),"
      "POINT (5.0012134888 7.9985313168),"
      "LINESTRING (3.0012135082 8.9985312634,"
      "4.0012134989 9.998531287600001,"
      "5.0012134891 8.998531314699999,4.0012134982 7.9985312908),"
      "POLYGON ((4.0012134982 7.9985312908,"
      "4.0012134986 8.998531289300001,"
      "3.0012135082 8.9985312634,2.0012135176 8.9985312369,"
      "2.0012135173 7.9985312385)))");
  EXPECT_EQ(wkbGeometryCollection, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x00\x00\x00\x00\x07\x00\x00\x00\x04\x00\x00\x00\x00\x01\x40\x14\x01"
      "\x3e\x1b\xe5\x25\xef\x40\x23\xff\x3f\x7f\x10\xbf\x30\x00\x00\x00\x00"
      "\x01\x40\x14\x01\x3e\x1b\xda\xd7\x1b\x40\x1f\xfe\x7e\xfe\x56\xc0\x53"
      "\x00\x00\x00\x00\x02\x00\x00\x00\x04\x40\x08\x02\x7c\x3a\x50\x42\x6a"
      "\x40\x21\xff\x3f\x7d\x60\xac\x52\x40\x10\x01\x3e\x1c\x88\x5b\x60\x40"
      "\x23\xff\x3f\x7e\x30\x8c\xaf\x40\x14\x01\x3e\x1b\xdf\xfe\x85\x40\x21"
      "\xff\x3f\x7f\x19\x56\x36\x40\x10\x01\x3e\x1c\x7c\x54\xbe\x40\x1f\xfe"
      "\x7e\xfc\x98\x13\x1d\x00\x00\x00\x00\x03\x00\x00\x00\x01\x00\x00\x00"
      "\x05\x40\x10\x01\x3e\x1c\x7c\x54\xbe\x40\x1f\xfe\x7e\xfc\x98\x13\x1d"
      "\x40\x10\x01\x3e\x1c\x83\x33\xf6\x40\x21\xff\x3f\x7e\x3f\x27\x06\x40"
      "\x08\x02\x7c\x3a\x50\x42\x6a\x40\x21\xff\x3f\x7d\x60\xac\x52\x40\x00"
      "\x02\x7c\x3b\x93\x3d\xb0\x40\x21\xff\x3f\x7c\x7d\x0a\x34\x40\x00\x02"
      "\x7c\x3b\x88\xee\xdc\x40\x1f\xfe\x7e\xf9\x15\x91\x48";

  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const std::string wkt_from_wkt = Wkt(*geom_from_wkt);
  const std::string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGeometryFactoryWkbWktTest, MultiPolygon) {
  auto geom_from_wkt = GeomFromWkt(
      "MULTIPOLYGON("
      "((10.0012134398567 2.99853145316592,10.001213439391 1.99853145478119,"
      "11.0012134299031 1.99853148052352,11.0012134303688 2.99853147896647)),"
      "((10.0012134398567 2.99853145316592,10.0012134400313 3.99853145200177,"
      "9.00121344957734 3.99853142627398,9.0012134493445 2.99853142749635)))");
  EXPECT_EQ(wkbMultiPolygon, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x01\x06\x00\x00\x00\x02\x00\x00\x00\x01\x03\x00\x00\x00\x01\x00\x00"
      "\x00\x04\x00\x00\x00\x00\x00\x49\x0c\x9f\x00\x24\x40\x00\x00\xfb\x0e"
      "\xfe\xfc\x07\x40\x00\x00\x45\x0c\x9f\x00\x24\x40\x00\x00\x65\x1e\xfc"
      "\xf9\xff\x3f\x00\x80\xf3\x0b\x9f\x00\x26\x40\x00\x00\x4e\x25\xfc\xf9"
      "\xff\x3f\x00\x80\xf7\x0b\x9f\x00\x26\x40\x00\x80\x71\x12\xfe\xfc\x07"
      "\x40\x01\x03\x00\x00\x00\x01\x00\x00\x00\x04\x00\x00\x00\x00\x00\x49"
      "\x0c\x9f\x00\x24\x40\x00\x00\xfb\x0e\xfe\xfc\x07\x40\x00\x80\x4a\x0c"
      "\x9f\x00\x24\x40\x00\x00\xd3\x0e\xfe\xfc\x0f\x40\x00\x80\x9c\x0c\x9f"
      "\x00\x22\x40\x00\x00\x5f\x0b\xfe\xfc\x0f\x40\x00\x80\x9a\x0c\x9f\x00"
      "\x22\x40\x00\x00\x89\x0b\xfe\xfc\x07\x40";

  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const std::string wkt_from_wkt = Wkt(*geom_from_wkt);
  const std::string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGeometryFactoryWkbWktTest, LineString) {
  auto geom_from_wkt = GeomFromWkt(
      "LINESTRING("
      "0.001213534618728 1.998531195873511,1.00121352536371 2.998531220131554,"
      "1.00121352635324 5.998531215489493)");
  EXPECT_EQ(wkbLineString, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x01\x02\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\xe0\xee\xe1\x53\x3f"
      "\x00\x00\xe5\xd8\xfb\xf9\xff\x3f\x00\x00\x3c\x79\xf8\x04\xf0\x3f\x00"
      "\x00\xb4\xef\xfd\xfc\x07\x40\x00\x00\x80\x79\xf8\x04\xf0\x3f\x00\x40"
      "\x8a\xf7\x7e\xfe\x17\x40";

  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const std::string wkt_from_wkt = Wkt(*geom_from_wkt);
  const std::string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGeometryFactoryWkbWktTest, Polygon) {
  auto geom_from_wkt = GeomFromWkt(
      "POLYGON("
      "(3.001213507319335 5.998531267236103,5.00121348828543 5.998531318837195,"
      "5.001213488576468 6.99853131796408,"
      "3.001213507552166 6.998531266246573))");
  EXPECT_EQ(wkbPolygon, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x01\x03\x00\x00\x00\x01\x00\x00\x00\x04\x00\x00\x00\x00\x00\x32\x3a"
      "\x7c\x02\x08\x40\x00\x40\x03\xfb\x7e\xfe\x17\x40\x00\x00\xd2\x1b\x3e"
      "\x01\x14\x40\x00\xc0\x79\xfe\x7e\xfe\x17\x40\x00\x00\xd7\x1b\x3e\x01"
      "\x14\x40\x00\xc0\x6a\xfe\x7e\xfe\x1b\x40\x00\x00\x3a\x3a\x7c\x02\x08"
      "\x40\x00\x40\xf2\xfa\x7e\xfe\x1b\x40";

  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const std::string wkt_from_wkt = Wkt(*geom_from_wkt);
  const std::string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

// TODO(schwehr): What was different about the polygons in 6.wkt and 7.wkt?

// The upstream 8.wkt POINT has unbalanced parens.  Why?
// https://trac.osgeo.org/gdal/ticket/6725

TEST(OgrGeometryFactoryWkbWktTest, Point) {
  auto geom_from_wkt = GeomFromWkt("POINT (2.0012135167 5.9985312409)");
  EXPECT_EQ(wkbPoint, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x00\x00\x00\x00\x01\x40\x00\x02\x7c\x3b\x74\x51\x34\x40\x17\xfe\x7e"
      "\xf9\x3e\xcc\x98";

  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const std::string wkt_from_wkt = Wkt(*geom_from_wkt);
  const std::string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

// TODO(schwehr): What is the reason for having 8.wkt, 9.wkt and 10.wkt?
// All POINTS, but what is special?
// And 11.wkt is another MULTIPOINT.  What is different from 1.wkt?

TEST(OgrGeometryFactoryWkt, OGRGeometryFactoryStrokeArcCrash) {
  // crash-11b07184f621765599968fd34949c4b58426de88
  const char kWkt[] =
      "COMPOUNDCURVE(CIRCULARSTRING(5 0V, 555e501 0, 0 00 0V, 1 01, 0 0)),";
  OGRGeometry* geomptr = nullptr;
  unique_ptr<char, autotest2::FreeDeleter> wkt_copy(strdup(kWkt));
  char* wkt_end = wkt_copy.get();
  EXPECT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&wkt_end, nullptr, &geomptr));
  auto geom = absl::WrapUnique<OGRGeometry>(geomptr);

  // Updated to point to the just past last character consumed.
  EXPECT_EQ(',', *wkt_end);

  EXPECT_EQ(wkbCompoundCurveZ, geom->getGeometryType());
  const char kExpected[] =
      "COMPOUNDCURVE Z (CIRCULARSTRING Z (5 0 0,inf 0.0 0,0 0 0,1 1 0,0 0 0))";
  EXPECT_EQ(kExpected, Wkt(*geom));
}

// =========================================================================
// Additional comprehensive tests for OGRGeometryFactory
// =========================================================================

using ::testing::NotNull;

TEST(OgrGeometryFactoryTest, CreateFromWkbAllTypesAndVariants) {
  const std::vector<std::string> wkts = {
      "POINT (10 20)",
      "POINT Z (10 20 30)",
      "POINT M (10 20 40)",
      "POINT ZM (10 20 30 40)",
      "POINT EMPTY",
      "LINESTRING (0 0, 1 1, 2 0)",
      "LINESTRING Z (0 0 1, 1 1 2, 2 0 3)",
      "LINESTRING M (0 0 4, 1 1 5, 2 0 6)",
      "LINESTRING ZM (0 0 1 4, 1 1 2 5, 2 0 3 6)",
      "LINESTRING EMPTY",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))",
      "POLYGON Z ((0 0 0, 10 0 0, 10 10 0, 0 10 0, 0 0 0))",
      "POLYGON M ((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1))",
      "POLYGON ZM ((0 0 0 1, 10 0 0 1, 10 10 0 1, 0 10 0 1, 0 0 0 1))",
      "POLYGON EMPTY",
      "MULTIPOINT ((1 2), (3 4))",
      "MULTIPOINT Z ((1 2 3), (3 4 5))",
      "MULTIPOINT M ((1 2 4), (3 4 6))",
      "MULTIPOINT ZM ((1 2 3 4), (3 4 5 6))",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
      "MULTILINESTRING Z ((0 0 1, 1 1 2), (2 2 3, 3 3 4))",
      "MULTILINESTRING M ((0 0 5, 1 1 6), (2 2 7, 3 3 8))",
      "MULTILINESTRING ZM ((0 0 1 5, 1 1 2 6), (2 2 3 7, 3 3 4 8))",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)))",
      "MULTIPOLYGON Z (((0 0 0, 1 0 0, 1 1 0, 0 1 0, 0 0 0)))",
      "MULTIPOLYGON M (((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1)))",
      "MULTIPOLYGON ZM (((0 0 0 1, 1 0 0 1, 1 1 0 1, 0 1 0 1, 0 0 0 1)))",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 1 1))",
      "GEOMETRYCOLLECTION Z (POINT Z (1 2 3))",
      "GEOMETRYCOLLECTION M (POINT M (1 2 4))",
      "GEOMETRYCOLLECTION ZM (POINT ZM (1 2 3 4))",
      "GEOMETRYCOLLECTION EMPTY",
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "CIRCULARSTRING Z (0 0 1, 1 1 2, 2 0 3)",
      "CIRCULARSTRING M (0 0 4, 1 1 5, 2 0 6)",
      "CIRCULARSTRING ZM (0 0 1 4, 1 1 2 5, 2 0 3 6)",
      "CIRCULARSTRING EMPTY",
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))",
      "COMPOUNDCURVE EMPTY",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "CURVEPOLYGON EMPTY",
      "MULTICURVE (CIRCULARSTRING (0 0, 1 1, 2 0))",
      "MULTICURVE EMPTY",
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)))",
      "MULTISURFACE EMPTY",
      "POLYHEDRALSURFACE Z (((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)))",
      "POLYHEDRALSURFACE EMPTY",
      "TIN Z (((0 0 0, 0 1 0, 1 0 0, 0 0 0)))",
      "TIN EMPTY",
      "TRIANGLE ((0 0, 0 1, 1 0, 0 0))",
      "TRIANGLE EMPTY",
  };

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  for (const auto& wkt : wkts) {
    OGRGeometry* geom = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(wkt.c_str(), &srs, &geom))
        << "Failed on: " << wkt;
    ASSERT_THAT(geom, NotNull());

    // Test both byte orders and variants
    for (OGRwkbByteOrder bo : {wkbNDR, wkbXDR}) {
      for (OGRwkbVariant variant :
           {wkbVariantIso, wkbVariantOldOgc, wkbVariantPostGIS1}) {
        const size_t wkbSize = geom->WkbSize();
        std::vector<unsigned char> buf(wkbSize);
        if (geom->exportToWkb(bo, buf.data(), variant) == OGRERR_NONE) {
          OGRGeometry* fromWkb = nullptr;
          size_t wkbBytesConsumed = 0;
          EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkb(
                                     buf.data(), &srs, &fromWkb, buf.size(),
                                     variant, wkbBytesConsumed));
          if (fromWkb) {
            EXPECT_EQ(wkbBytesConsumed, buf.size());
            OGRGeometryFactory::destroyGeometry(fromWkb);
          }

          // Test C APIs
          OGRGeometryH hGeom = nullptr;
          if (OGR_G_CreateFromWkb(
                  buf.data(), OGRSpatialReference::ToHandle(&srs), &hGeom,
                  static_cast<int>(buf.size())) == OGRERR_NONE) {
            OGR_G_DestroyGeometry(hGeom);
            hGeom = nullptr;
          }

          if (OGR_G_CreateFromWkbEx(buf.data(),
                                    OGRSpatialReference::ToHandle(&srs), &hGeom,
                                    buf.size()) == OGRERR_NONE) {
            OGR_G_DestroyGeometry(hGeom);
            hGeom = nullptr;
          }
        }
      }
    }
    OGRGeometryFactory::destroyGeometry(geom);
  }

  // Error cases
  OGRGeometry* errGeom = nullptr;
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGRGeometryFactory::createFromWkb(nullptr, nullptr, &errGeom, 0));
  unsigned char smallBuf[4] = {1, 1, 0, 0};
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGRGeometryFactory::createFromWkb(smallBuf, nullptr, &errGeom, 4));

  unsigned char invalidTypeBuf[10] = {1, 0xFF, 0xFF, 0, 0,
                                      0, 0,    0,    0, 0};  // type 65535
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGRGeometryFactory::createFromWkb(invalidTypeBuf, nullptr, &errGeom,
                                              sizeof(invalidTypeBuf)));

  // Truncated WKB
  unsigned char ptBuf[21] = {1, 1, 0, 0, 0};  // wkbPoint little endian
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGRGeometryFactory::createFromWkb(ptBuf, nullptr, &errGeom, 10));

  // C API error cases
  OGRGeometryH hErr = nullptr;
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGR_G_CreateFromWkb(nullptr, nullptr, &hErr, 0));
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGR_G_CreateFromWkbEx(nullptr, nullptr, &hErr, 0));
}

TEST(OgrGeometryFactoryTest, CreateFromWktErrorsAndEnvelope) {
  OGRGeometry* geom = nullptr;

  // Null or empty string
  const char* nullWkt = nullptr;
  EXPECT_NE(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(nullWkt, nullptr, &geom));
  const char* emptyStr = "";
  EXPECT_NE(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&emptyStr, nullptr, &geom));
  const char* spaceStr = "   ";
  EXPECT_NE(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&spaceStr, nullptr, &geom));

  // Unrecognized token
  const char* badToken = "INVALID_GEOM_TYPE (0 0)";
  EXPECT_NE(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&badToken, nullptr, &geom));

  // Missing bracket
  const char* missingBracket = "POINT 0 0";
  EXPECT_NE(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&missingBracket, nullptr, &geom));

  // Unclosed bracket
  const char* unclosed = "POINT (0 0";
  EXPECT_NE(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&unclosed, nullptr, &geom));

  // Extra trailing characters
  const char* trailing = "POINT (0 0) EXTRA";
  EXPECT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(&trailing, nullptr, &geom));
  ASSERT_THAT(geom, NotNull());
  EXPECT_STREQ(trailing, "EXTRA");
  OGRGeometryFactory::destroyGeometry(geom);

  // C API: OGR_G_CreateFromWkt
  OGRGeometryH hGeom = nullptr;
  const char* cWkt = "POINT (5 10)";
  char* pszWkt = const_cast<char*>(cWkt);
  EXPECT_EQ(OGRERR_NONE, OGR_G_CreateFromWkt(&pszWkt, nullptr, &hGeom));
  ASSERT_THAT(hGeom, NotNull());
  OGR_G_DestroyGeometry(hGeom);

  // OGR_G_CreateFromEnvelope
  OGRGeometryH hPoly = OGR_G_CreateFromEnvelope(0.0, 0.0, 10.0, 10.0, nullptr);
  ASSERT_THAT(hPoly, NotNull());
  OGR_G_DestroyGeometry(hPoly);

  // Create from envelope with MinX > MaxX
  OGRGeometryH hBadEnv =
      OGR_G_CreateFromEnvelope(10.0, 0.0, 0.0, 10.0, nullptr);
  ASSERT_THAT(hBadEnv, NotNull());
  OGR_G_DestroyGeometry(hBadEnv);
}

TEST(OgrGeometryFactoryTest, CreateAndDestroyGeometryAllTypes) {
  const std::vector<OGRwkbGeometryType> types = {
      wkbPoint,
      wkbPoint25D,
      wkbPointM,
      wkbPointZM,
      wkbLineString,
      wkbLineString25D,
      wkbLineStringM,
      wkbLineStringZM,
      wkbPolygon,
      wkbPolygon25D,
      wkbPolygonM,
      wkbPolygonZM,
      wkbMultiPoint,
      wkbMultiPoint25D,
      wkbMultiPointM,
      wkbMultiPointZM,
      wkbMultiLineString,
      wkbMultiLineString25D,
      wkbMultiLineStringM,
      wkbMultiLineStringZM,
      wkbMultiPolygon,
      wkbMultiPolygon25D,
      wkbMultiPolygonM,
      wkbMultiPolygonZM,
      wkbGeometryCollection,
      wkbGeometryCollection25D,
      wkbGeometryCollectionM,
      wkbGeometryCollectionZM,
      wkbCircularString,
      wkbCircularStringZ,
      wkbCircularStringM,
      wkbCircularStringZM,
      wkbCompoundCurve,
      wkbCompoundCurveZ,
      wkbCompoundCurveM,
      wkbCompoundCurveZM,
      wkbCurvePolygon,
      wkbCurvePolygonZ,
      wkbCurvePolygonM,
      wkbCurvePolygonZM,
      wkbMultiCurve,
      wkbMultiCurveZ,
      wkbMultiCurveM,
      wkbMultiCurveZM,
      wkbMultiSurface,
      wkbMultiSurfaceZ,
      wkbMultiSurfaceM,
      wkbMultiSurfaceZM,
      wkbPolyhedralSurface,
      wkbPolyhedralSurfaceZ,
      wkbPolyhedralSurfaceM,
      wkbPolyhedralSurfaceZM,
      wkbTIN,
      wkbTINZ,
      wkbTINM,
      wkbTINZM,
      wkbTriangle,
      wkbTriangleZ,
      wkbTriangleM,
      wkbTriangleZM,
      wkbNone,
      wkbUnknown,
  };

  for (auto t : types) {
    OGRGeometry* g = OGRGeometryFactory::createGeometry(t);
    if (t != wkbNone && t != wkbUnknown) {
      ASSERT_THAT(g, NotNull()) << "Failed on type " << t;
    }
    OGRGeometryFactory::destroyGeometry(g);

    // C API
    OGRGeometryH h = OGR_G_CreateGeometry(t);
    if (t != wkbNone && t != wkbUnknown) {
      ASSERT_THAT(h, NotNull());
    }
    OGR_G_DestroyGeometry(h);
  }

  // Unsupported type returns nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::createGeometry(
                         static_cast<OGRwkbGeometryType>(99999)));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometry(static_cast<OGRwkbGeometryType>(99999)));

  // destroyGeometry with nullptr is safe
  OGRGeometryFactory::destroyGeometry(nullptr);
  OGR_G_DestroyGeometry(nullptr);
}

TEST(OgrGeometryFactoryTest, ForceToPolygonAndMultiPolygon) {
  // forceToPolygon: nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::forceToPolygon(nullptr));
  EXPECT_EQ(nullptr, OGR_G_ForceToPolygon(nullptr));

  // Polygon -> self
  {
    auto poly = std::make_unique<OGRPolygon>();
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(poly.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // LinearRing -> Polygon
  {
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(10, 0);
    lr->addPoint(10, 10);
    lr->addPoint(0, 10);
    lr->addPoint(0, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(lr.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // LineString (closed) -> Polygon
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(10, 0);
    ls->addPoint(10, 10);
    ls->addPoint(0, 10);
    ls->addPoint(0, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // LineString (closed) -> Polygon
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(10, 0);
    ls->addPoint(10, 10);
    ls->addPoint(0, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // LineString (open) -> returned unmodified
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(10, 0);
    ls->addPoint(10, 10);
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiPolygon: 1 child
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    mp->addGeometryDirectly(poly.release());
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(mp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiPolygon: empty
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(mp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    EXPECT_TRUE(res->IsEmpty());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiPolygon: multiple polygons -> returns first
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    for (int i = 0; i < 2; ++i) {
      auto poly = std::make_unique<OGRPolygon>();
      auto lr = std::make_unique<OGRLinearRing>();
      lr->addPoint(i * 10, 0);
      lr->addPoint(i * 10 + 1, 0);
      lr->addPoint(i * 10 + 1, 1);
      lr->addPoint(i * 10, 0);
      poly->addRingDirectly(lr.release());
      mp->addGeometryDirectly(poly.release());
    }
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(mp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // CurvePolygon -> Polygon
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cp->addRingDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(cp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiSurface -> Polygon
  {
    auto ms = std::make_unique<OGRMultiSurface>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    ms->addGeometryDirectly(poly.release());
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(ms.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // Point -> returns self
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(pt.release());
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // GeometryCollection with polygon
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    gc->addGeometryDirectly(poly.release());
    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // forceToMultiPolygon: nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::forceToMultiPolygon(nullptr));
  EXPECT_EQ(nullptr, OGR_G_ForceToMultiPolygon(nullptr));

  // Polygon -> MultiPolygon
  {
    auto poly = std::make_unique<OGRPolygon>();
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(poly.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiSurface -> MultiPolygon
  {
    auto ms = std::make_unique<OGRMultiSurface>();
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(ms.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // LineString -> MultiPolygon
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 0);
    ls->addPoint(1, 1);
    ls->addPoint(0, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // Point -> returned unmodified
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(pt.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }
}

TEST(OgrGeometryFactoryTest, ForceToMultiPointAndMultiLineString) {
  // forceToMultiPoint: nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::forceToMultiPoint(nullptr));
  EXPECT_EQ(nullptr, OGR_G_ForceToMultiPoint(nullptr));

  // Point -> MultiPoint
  {
    auto pt = std::make_unique<OGRPoint>(10, 20);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPoint(pt.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPoint, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiPoint -> self
  {
    auto mp = std::make_unique<OGRMultiPoint>();
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPoint(mp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPoint, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // LineString -> returned unmodified
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(1, 2);
    ls->addPoint(3, 4);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPoint(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // Polygon -> returned unmodified
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPoint(poly.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // GeometryCollection -> MultiPoint
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRPoint(1, 2));
    gc->addGeometryDirectly(new OGRPoint(3, 4));
    OGRGeometry* res = OGRGeometryFactory::forceToMultiPoint(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPoint, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // forceToMultiLineString: nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::forceToMultiLineString(nullptr));
  EXPECT_EQ(nullptr, OGR_G_ForceToMultiLineString(nullptr));

  // LineString -> MultiLineString
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiCurve -> MultiLineString
  {
    auto mc = std::make_unique<OGRMultiCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    mc->addGeometryDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(mc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // Polygon -> MultiLineString
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceToMultiLineString(poly.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // MultiPolygon -> MultiLineString
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    mp->addGeometryDirectly(poly.release());
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(mp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }

  // Incompatible types like Point are returned unmodified
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(pt.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
    OGRGeometryFactory::destroyGeometry(res);
  }
}

TEST(OgrGeometryFactoryTest, RemoveLowerDimensionSubGeoms) {
  EXPECT_EQ(nullptr, OGRGeometryFactory::removeLowerDimensionSubGeoms(nullptr));
  EXPECT_EQ(nullptr, OGR_G_RemoveLowerDimensionSubGeoms(nullptr));

  // Single geometries returned as clone
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(pt.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
    delete res;
  }

  // GeometryCollection with Polygon, LineString, and Point -> keeps only
  // Polygon
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    gc->addGeometryDirectly(poly.release());
    gc->addGeometryDirectly(new OGRLineString());
    gc->addGeometryDirectly(new OGRPoint(5, 5));

    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    delete res;

    // C API
    OGRGeometryH hRes =
        OGR_G_RemoveLowerDimensionSubGeoms(OGRGeometry::ToHandle(gc.get()));
    ASSERT_THAT(hRes, NotNull());
    OGR_G_DestroyGeometry(hRes);
  }

  // GeometryCollection with MultiPolygon + LineString -> GeometryCollection
  // containing MultiPolygon
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto mp = std::make_unique<OGRMultiPolygon>();
    gc->addGeometryDirectly(mp.release());
    gc->addGeometryDirectly(new OGRLineString());

    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
    delete res;
  }

  // GeometryCollection with LineString + Point -> LineString
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRLineString());
    gc->addGeometryDirectly(new OGRPoint(1, 1));

    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // GeometryCollection with only Points -> MultiPoint
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRPoint(1, 1));
    gc->addGeometryDirectly(new OGRPoint(2, 2));

    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPoint, res->getGeometryType());
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, OrganizePolygonsComprehensive) {
  // nPolygonCount == 0 -> empty Polygon
  {
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(nullptr, 0, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    EXPECT_TRUE(bValid);
    delete res;
  }

  // nPolygonCount == 1 with valid polygon
  {
    OGRGeometry* poly = nullptr;
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &poly);
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(&poly, 1, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    EXPECT_TRUE(bValid);
    delete res;
  }

  // nPolygonCount == 1 with non-polygon (warning branch)
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRGeometry* pt = new OGRPoint(1, 2);
    int bValid = TRUE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(&pt, 1, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    EXPECT_FALSE(bValid);
    delete res;
  }

  // Two disjoint outer polygons -> MultiPolygon
  {
    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))", nullptr, &polys[1]);
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // 1 outer + 1 hole -> 1 Polygon with 2 rings
  {
    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
                                      nullptr, &polys[1]);
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    EXPECT_EQ(2, res->toPolygon()->getNumInteriorRings() + 1);
    delete res;
  }

  // 1 outer + 1 hole + 1 island in hole -> MultiPolygon
  {
    OGRGeometry* polys[3];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
                                      nullptr, &polys[1]);
    OGRGeometryFactory::createFromWkt("POLYGON ((4 4, 6 4, 6 6, 4 6, 4 4))",
                                      nullptr, &polys[2]);
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 3, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // CurvePolygon input
  {
    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt(
        "CURVEPOLYGON (CIRCULARSTRING (0 0, 5 5, 10 0, 5 -5, 0 0))", nullptr,
        &polys[0]);
    OGRGeometryFactory::createFromWkt(
        "CURVEPOLYGON (CIRCULARSTRING (2 0, 5 3, 8 0, 5 -3, 2 0))", nullptr,
        &polys[1]);
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // Options: METHOD=SKIP, METHOD=ONLY_CCW, METHOD=CCW_INNER
  {
    const char* optionsSkip[] = {"METHOD=SKIP", nullptr};
    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
                                      nullptr, &polys[1]);
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, nullptr, optionsSkip);
    ASSERT_THAT(res, NotNull());
    delete res;
  }
  {
    const char* optionsCCW[] = {"METHOD=ONLY_CCW", nullptr};
    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
                                      nullptr, &polys[1]);
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, nullptr, optionsCCW);
    ASSERT_THAT(res, NotNull());
    delete res;
  }
  {
    const char* optionsCCWInner[] = {"METHOD=CCW_INNER", nullptr};
    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
                                      nullptr, &polys[1]);
    OGRGeometry* res = OGRGeometryFactory::organizePolygons(polys, 2, nullptr,
                                                            optionsCCWInner);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // Non-polygon single geometry -> bValid is false, returns empty Polygon
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRGeometry* polys[1];
    polys[0] = new OGRPoint(1, 2);
    int bValid = TRUE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 1, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_FALSE(bValid);
    delete res;
  }
  // Non-polygon in multi-geometry array -> ignored/deleted
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRGeometry* polys[2];
    polys[0] = new OGRPoint(1, 2);
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[1]);
    int bValid = TRUE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // Config options: OGR_ORGANIZE_POLYGONS and OGR_DEBUG_ORGANIZE_POLYGONS
  {
    CPLSetConfigOption("OGR_DEBUG_ORGANIZE_POLYGONS", "YES");
    CPLSetConfigOption("OGR_ORGANIZE_POLYGONS", "DEFAULT");
    auto cleanup = absl::MakeCleanup([] {
      CPLSetConfigOption("OGR_DEBUG_ORGANIZE_POLYGONS", nullptr);
      CPLSetConfigOption("OGR_ORGANIZE_POLYGONS", nullptr);
    });

    OGRGeometry* polys[2];
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))", nullptr, &polys[1]);
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, nullptr, nullptr);
    ASSERT_THAT(res, NotNull());
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, CreateFromGMLAndGEOS) {
  // Valid GML
  const char* gml =
      "<gml:Point><gml:coordinates>10,20</gml:coordinates></gml:Point>";
  OGRGeometry* geom = OGRGeometryFactory::createFromGML(gml);
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbPoint, geom->getGeometryType());
  delete geom;

  // Invalid GML
  EXPECT_EQ(nullptr, OGRGeometryFactory::createFromGML(nullptr));
  CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, OGRGeometryFactory::createFromGML("<invalid>"));

  // haveGEOS
  bool bHaveGEOS = OGRGeometryFactory::haveGEOS();
  (void)bHaveGEOS;
#ifdef HAVE_GEOS
  EXPECT_TRUE(bHaveGEOS);
  // createFromGEOS
  {
    GEOSContextHandle_t hGEOSCtxt = OGRGeometry::createGEOSContext();
    if (hGEOSCtxt) {
      auto pt = std::make_unique<OGRPoint>(15.0, 25.0);
      GEOSGeom geosGeom = pt->exportToGEOS(hGEOSCtxt);
      if (geosGeom) {
        OGRGeometry* imported =
            OGRGeometryFactory::createFromGEOS(hGEOSCtxt, geosGeom);
        ASSERT_THAT(imported, NotNull());
        EXPECT_EQ(wkbPoint, imported->getGeometryType());
        GEOSGeom_destroy_r(hGEOSCtxt, geosGeom);
        delete imported;
      }
      OGRGeometry::freeGEOSContext(hGEOSCtxt);
    }
  }
#endif
}

TEST(OgrGeometryFactoryTest, CreateFromFgfBinaryFormat) {
  // Type 0: None
  {
    int32_t fgfNone[1] = {0};
    OGRGeometry* geom = nullptr;
    int fgfBytesConsumed = 0;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromFgf(
                               fgfNone, nullptr, &geom, 4, &fgfBytesConsumed));
    EXPECT_EQ(nullptr, geom);
    EXPECT_EQ(4, fgfBytesConsumed);
  }

  // Type 1: Point 2D
  {
    struct {
      int32_t type = 1;
      int32_t dim = 0;
      double x = 10.0;
      double y = 20.0;
    } fgfPoint;
    OGRGeometry* geom = nullptr;
    int fgfBytesConsumed = 0;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromFgf(
                               &fgfPoint, nullptr, &geom, sizeof(fgfPoint),
                               &fgfBytesConsumed));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
    EXPECT_EQ(sizeof(fgfPoint), fgfBytesConsumed);
    delete geom;

    // C API: OGR_G_CreateFromFgf
    OGRGeometryH hGeom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGR_G_CreateFromFgf(&fgfPoint, nullptr, &hGeom, sizeof(fgfPoint),
                                  &fgfBytesConsumed));
    ASSERT_THAT(hGeom, NotNull());
    OGR_G_DestroyGeometry(hGeom);
  }

  // Type 1: Point 3D (Z)
  {
    struct {
      int32_t type = 1;
      int32_t dim = 1;
      double x = 10.0;
      double y = 20.0;
      double z = 30.0;
    } fgfPointZ;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(&fgfPointZ, nullptr, &geom,
                                                sizeof(fgfPointZ), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbPoint25D, geom->getGeometryType());
    delete geom;
  }

  // Type 2: LineString 2D
  {
    struct __attribute__((packed)) {
      int32_t type = 2;
      int32_t dim = 0;
      int32_t numPoints = 2;
      double x0 = 0.0, y0 = 0.0;
      double x1 = 1.0, y1 = 1.0;
    } fgfLine;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(&fgfLine, nullptr, &geom,
                                                sizeof(fgfLine), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    delete geom;
  }

  // Type 3: Polygon 2D
  {
    struct {
      int32_t type = 3;
      int32_t dim = 0;
      int32_t numRings = 1;
      int32_t numPoints = 4;
      double x0 = 0, y0 = 0;
      double x1 = 1, y1 = 0;
      double x2 = 1, y2 = 1;
      double x3 = 0, y3 = 0;
    } fgfPoly;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(&fgfPoly, nullptr, &geom,
                                                sizeof(fgfPoly), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
    delete geom;
  }

  // Type 4: MultiPoint
  {
    struct Pt2D {
      int32_t type = 1;
      int32_t dim = 0;
      double x = 5.0, y = 6.0;
    };
    struct {
      int32_t type = 4;
      int32_t numGeoms = 1;
      Pt2D child;
    } fgfMP;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromFgf(
                               &fgfMP, nullptr, &geom, sizeof(fgfMP), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
    delete geom;
  }

  // Type 5: MultiLineString
  {
    struct __attribute__((packed)) LS {
      int32_t type = 2;
      int32_t dim = 0;
      int32_t numPoints = 2;
      double x0 = 0, y0 = 0, x1 = 1, y1 = 1;
    };
    struct __attribute__((packed)) {
      int32_t type = 5;
      int32_t numGeoms = 1;
      LS child;
    } fgfMLS;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(&fgfMLS, nullptr, &geom,
                                                sizeof(fgfMLS), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbMultiLineString, geom->getGeometryType());
    delete geom;
  }

  // Type 6: MultiPolygon
  {
    struct Poly {
      int32_t type = 3;
      int32_t dim = 0;
      int32_t numRings = 1;
      int32_t numPoints = 4;
      double x0 = 0, y0 = 0, x1 = 1, y1 = 0, x2 = 1, y2 = 1, x3 = 0, y3 = 0;
    };
    struct {
      int32_t type = 6;
      int32_t numGeoms = 1;
      Poly child;
    } fgfMPoly;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(&fgfMPoly, nullptr, &geom,
                                                sizeof(fgfMPoly), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
    delete geom;
  }

  // Type 7: GeometryCollection
  {
    struct Pt2D {
      int32_t type = 1;
      int32_t dim = 0;
      double x = 1.0, y = 2.0;
    };
    struct {
      int32_t type = 7;
      int32_t numGeoms = 1;
      Pt2D child;
    } fgfGC;
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromFgf(
                               &fgfGC, nullptr, &geom, sizeof(fgfGC), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbGeometryCollection, geom->getGeometryType());
    delete geom;
  }

  // Error cases in FGF
  OGRGeometry* errGeom = nullptr;
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGRGeometryFactory::createFromFgf(nullptr, nullptr, &errGeom, 0,
                                              nullptr));
  int32_t badType[2] = {99, 0};
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGRGeometryFactory::createFromFgf(badType, nullptr, &errGeom, 8,
                                              nullptr));
  int32_t badDim[2] = {1, 5};  // dim 5 is invalid
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRGeometryFactory::createFromFgf(
                                     badDim, nullptr, &errGeom, 8, nullptr));
  int32_t truncPt[2] = {1, 0};  // truncated point (needs 8 + 16 = 24 bytes)
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            OGRGeometryFactory::createFromFgf(truncPt, nullptr, &errGeom, 8,
                                              nullptr));
}

TEST(OgrGeometryFactoryTest, TransformWithOptionsAndDateLine) {
  // 1. WRAPDATELINE=YES without CT
  {
    const char* options[] = {"WRAPDATELINE=YES", nullptr};
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt("LINESTRING (179 0, -179 0)", nullptr,
                                      &geom);
    ASSERT_THAT(geom, NotNull());

    OGRGeometry* transformed = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(transformed, NotNull());
    EXPECT_EQ(wkbMultiLineString, transformed->getGeometryType());
    delete geom;
    delete transformed;
  }

  // Polygon crossing dateline
  {
    const char* options[] = {"WRAPDATELINE=YES", "DATELINEOFFSET=15", nullptr};
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((175 -10, -175 -10, -175 10, 175 10, 175 -10))", nullptr,
        &geom);
    ASSERT_THAT(geom, NotNull());

    OGRGeometry* transformed = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(transformed, NotNull());
    delete geom;
    delete transformed;
  }

  // Polygon NOT crossing dateline
  {
    const char* options[] = {"WRAPDATELINE=YES", nullptr};
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &geom);
    ASSERT_THAT(geom, NotNull());

    OGRGeometry* transformed = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(transformed, NotNull());
    delete geom;
    delete transformed;
  }

  // Point with WRAPDATELINE=YES
  {
    const char* options[] = {"WRAPDATELINE=YES", nullptr};
    auto pt = std::make_unique<OGRPoint>(10, 20);
    OGRGeometry* transformed = OGRGeometryFactory::transformWithOptions(
        pt.get(), nullptr, const_cast<char**>(options));
    ASSERT_THAT(transformed, NotNull());
    EXPECT_EQ(wkbPoint, transformed->getGeometryType());
    delete transformed;
  }

  // 2. Polar transformations with CT
  {
    OGRSpatialReference srsNorthPole, srsWGS84;
    srsNorthPole.importFromEPSG(3995);  // WGS 84 / Arctic Polar Stereographic
    srsWGS84.importFromEPSG(4326);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNorthPole, &srsWGS84));
    if (ct) {
      // Polygon containing North Pole
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((-50000 -50000, 50000 -50000, 50000 50000, -50000 50000, "
          "-50000 -50000))",
          &srsNorthPole, &geom);
      if (geom) {
        OGRGeometry* transformed =
            OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
        ASSERT_THAT(transformed, NotNull());
        delete geom;
        delete transformed;
      }

      // LineString passing through pole
      OGRGeometry* line = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (-100000 0, 100000 0)",
                                        &srsNorthPole, &line);
      if (line) {
        OGRGeometry* transformed =
            OGRGeometryFactory::transformWithOptions(line, ct.get(), nullptr);
        ASSERT_THAT(transformed, NotNull());
        delete line;
        delete transformed;
      }
    }
  }

  // South Pole transformation
  {
    OGRSpatialReference srsSouthPole, srsWGS84;
    srsSouthPole.importFromEPSG(
        3031);  // WGS 84 / Antarctic Polar Stereographic
    srsWGS84.importFromEPSG(4326);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsSouthPole, &srsWGS84));
    if (ct) {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((-50000 -50000, 50000 -50000, 50000 50000, -50000 50000, "
          "-50000 -50000))",
          &srsSouthPole, &geom);
      if (geom) {
        OGRGeometry* transformed =
            OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
        ASSERT_THAT(transformed, NotNull());
        delete geom;
        delete transformed;
      }
    }
  }

  // Antimeridian UTM zone 60N to WGS84
  {
    OGRSpatialReference srsUTM60, srsWGS84;
    srsUTM60.importFromEPSG(32660);
    srsWGS84.importFromEPSG(4326);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsUTM60, &srsWGS84));
    if (ct) {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((400000 0, 600000 0, 600000 200000, 400000 200000, 400000 "
          "0))",
          &srsUTM60, &geom);
      if (geom) {
        OGRGeometry* transformed =
            OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
        ASSERT_THAT(transformed, NotNull());
        delete geom;
        delete transformed;
      }
    }
  }

  // Standard transformation: EPSG:4326 to EPSG:3857
  {
    OGRSpatialReference srsWGS84, srs3857;
    srsWGS84.importFromEPSG(4326);
    srs3857.importFromEPSG(3857);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
    ASSERT_THAT(ct, NotNull());

    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    OGRGeometry* transformed =
        OGRGeometryFactory::transformWithOptions(pt.get(), ct.get(), nullptr);
    ASSERT_THAT(transformed, NotNull());
    EXPECT_EQ(wkbPoint, transformed->getGeometryType());
    delete transformed;
  }

  // 3. OGRGeomTransformer C API
  {
    OGRSpatialReference srsWGS84, srs3857;
    srsWGS84.importFromEPSG(4326);
    srs3857.importFromEPSG(3857);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
    ASSERT_THAT(ct, NotNull());

    OGRGeomTransformerH hTransformer = OGR_GeomTransformer_Create(
        OGRCoordinateTransformation::ToHandle(ct.get()), nullptr);
    ASSERT_THAT(hTransformer, NotNull());

    OGRGeometryH hPt = OGR_G_CreateGeometry(wkbPoint);
    OGR_G_SetPoint_2D(hPt, 0, 10.0, 20.0);

    OGRGeometryH hTransformed =
        OGR_GeomTransformer_Transform(hTransformer, hPt);
    ASSERT_THAT(hTransformed, NotNull());

    OGR_G_DestroyGeometry(hPt);
    OGR_G_DestroyGeometry(hTransformed);
    OGR_GeomTransformer_Destroy(hTransformer);
  }
}

TEST(OgrGeometryFactoryTest, ArcApproximations) {
  // GetDefaultArcStepSize
  double defaultStep = OGRGeometryFactory::GetDefaultArcStepSize();
  EXPECT_GT(defaultStep, 0.0);

  // approximateArcAngles
  // 1. Full circle (0 to 360)
  OGRGeometry* circle = OGRGeometryFactory::approximateArcAngles(
      0.0, 0.0, 0.0, 10.0, 10.0, 0.0, 0.0, 360.0, 10.0, true);
  ASSERT_THAT(circle, NotNull());
  EXPECT_GT(circle->toLineString()->getNumPoints(), 30);
  delete circle;

  // 2. Partial arc (30 to 120) with rotation
  OGRGeometry* arc = OGRGeometryFactory::approximateArcAngles(
      5.0, 5.0, 2.0, 10.0, 5.0, 45.0, 30.0, 120.0, 5.0, true);
  ASSERT_THAT(arc, NotNull());
  EXPECT_GT(arc->toLineString()->getNumPoints(), 5);
  delete arc;

  // 3. Reversed arc (120 to 30)
  OGRGeometry* revArc = OGRGeometryFactory::approximateArcAngles(
      0.0, 0.0, 0.0, 10.0, 10.0, 0.0, 120.0, 30.0, 15.0, false);
  ASSERT_THAT(revArc, NotNull());
  delete revArc;

  // 4. Default step size (0.0)
  OGRGeometry* defArc = OGRGeometryFactory::approximateArcAngles(
      0.0, 0.0, 0.0, 10.0, 10.0, 0.0, 0.0, 90.0, 0.0, true);
  ASSERT_THAT(defArc, NotNull());
  delete defArc;

  // 5. Zero radius (degenerate)
  OGRGeometry* degenArc = OGRGeometryFactory::approximateArcAngles(
      0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 90.0, 10.0, true);
  ASSERT_THAT(degenArc, NotNull());
  delete degenArc;

  // C API: OGR_G_ApproximateArcAngles
  OGRGeometryH hArc = OGR_G_ApproximateArcAngles(0.0, 0.0, 0.0, 10.0, 10.0, 0.0,
                                                 0.0, 180.0, 10.0);
  ASSERT_THAT(hArc, NotNull());
  OGR_G_DestroyGeometry(hArc);
}

TEST(OgrGeometryFactoryTest, ForceToLineString) {
  // nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::forceToLineString(nullptr));
  EXPECT_EQ(nullptr, OGR_G_ForceToLineString(nullptr));

  // LineString -> self
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(ls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // LinearRing -> LineString
  {
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(lr.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // CircularString -> LineString
  {
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(cs.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // CompoundCurve -> LineString
  {
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cc->addCurveDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(cc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // Polygon -> exterior ring as LineString
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(poly.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // MultiLineString -> LineString
  {
    auto mls = std::make_unique<OGRMultiLineString>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mls->addGeometryDirectly(ls.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(mls.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // MultiCurve -> LineString
  {
    auto mc = std::make_unique<OGRMultiCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    mc->addGeometryDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(mc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // MultiPolygon -> LineString
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    mp->addGeometryDirectly(poly.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(mp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // Empty GeometryCollection -> empty LineString
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    EXPECT_TRUE(res->IsEmpty());
    delete res;
  }

  // GeometryCollection -> LineString
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    gc->addGeometryDirectly(ls.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // Incompatible types like Point are returned unmodified
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(pt.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, ForceToExtensive) {
  // nullptr -> nullptr
  EXPECT_EQ(nullptr, OGRGeometryFactory::forceTo(nullptr, wkbPolygon));
  EXPECT_EQ(nullptr, OGR_G_ForceTo(nullptr, wkbPolygon, nullptr));

  // Unknown target type returns self
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res = OGRGeometryFactory::forceTo(pt.release(), wkbUnknown);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
    delete res;
  }

  // Empty geometry conversion
  {
    auto emptyPt = std::make_unique<OGRPoint>();
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(emptyPt.release(), wkbPolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    EXPECT_TRUE(res->IsEmpty());
    delete res;
  }

  // Dimensional promotion / demotion (2D to 3D and back)
  {
    auto pt2d = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* pt3d =
        OGRGeometryFactory::forceTo(pt2d.release(), wkbPoint25D);
    ASSERT_THAT(pt3d, NotNull());
    EXPECT_EQ(wkbPoint25D, pt3d->getGeometryType());

    OGRGeometry* ptBack2d = OGRGeometryFactory::forceTo(pt3d, wkbPoint);
    ASSERT_THAT(ptBack2d, NotNull());
    EXPECT_EQ(wkbPoint, ptBack2d->getGeometryType());
    delete ptBack2d;
  }

  // Point to MultiPoint
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res = OGRGeometryFactory::forceTo(pt.release(), wkbMultiPoint);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPoint, res->getGeometryType());
    delete res;
  }

  // MultiPoint to Point
  {
    auto mp = std::make_unique<OGRMultiPoint>();
    mp->addGeometryDirectly(new OGRPoint(1, 2));
    OGRGeometry* res = OGRGeometryFactory::forceTo(mp.release(), wkbPoint);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
    delete res;
  }

  // LineString conversions
  {
    // LineString to MultiLineString
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    OGRGeometry* mls =
        OGRGeometryFactory::forceTo(ls.release(), wkbMultiLineString);
    ASSERT_THAT(mls, NotNull());
    EXPECT_EQ(wkbMultiLineString, mls->getGeometryType());

    // MultiLineString to LineString
    OGRGeometry* lsBack = OGRGeometryFactory::forceTo(mls, wkbLineString);
    ASSERT_THAT(lsBack, NotNull());
    EXPECT_EQ(wkbLineString, lsBack->getGeometryType());

    // LineString to CompoundCurve
    OGRGeometry* cc = OGRGeometryFactory::forceTo(lsBack, wkbCompoundCurve);
    ASSERT_THAT(cc, NotNull());
    EXPECT_EQ(wkbCompoundCurve, cc->getGeometryType());

    // CompoundCurve to LineString
    OGRGeometry* ls2 = OGRGeometryFactory::forceTo(cc, wkbLineString);
    ASSERT_THAT(ls2, NotNull());
    EXPECT_EQ(wkbLineString, ls2->getGeometryType());

    // LineString to Polygon (closed)
    ls2->toLineString()->addPoint(1, 0);
    ls2->toLineString()->addPoint(0, 0);
    OGRGeometry* poly = OGRGeometryFactory::forceTo(ls2, wkbPolygon);
    ASSERT_THAT(poly, NotNull());
    EXPECT_EQ(wkbPolygon, poly->getGeometryType());

    // Polygon to CurvePolygon
    OGRGeometry* cp = OGRGeometryFactory::forceTo(poly, wkbCurvePolygon);
    ASSERT_THAT(cp, NotNull());
    EXPECT_EQ(wkbCurvePolygon, cp->getGeometryType());

    // CurvePolygon to Polygon
    OGRGeometry* polyBack = OGRGeometryFactory::forceTo(cp, wkbPolygon);
    ASSERT_THAT(polyBack, NotNull());
    EXPECT_EQ(wkbPolygon, polyBack->getGeometryType());

    // Polygon to MultiPolygon
    OGRGeometry* mpoly = OGRGeometryFactory::forceTo(polyBack, wkbMultiPolygon);
    ASSERT_THAT(mpoly, NotNull());
    EXPECT_EQ(wkbMultiPolygon, mpoly->getGeometryType());

    // MultiPolygon to Polygon
    OGRGeometry* polySingle = OGRGeometryFactory::forceTo(mpoly, wkbPolygon);
    ASSERT_THAT(polySingle, NotNull());
    EXPECT_EQ(wkbPolygon, polySingle->getGeometryType());

    // Polygon to MultiSurface
    OGRGeometry* ms = OGRGeometryFactory::forceTo(polySingle, wkbMultiSurface);
    ASSERT_THAT(ms, NotNull());
    EXPECT_EQ(wkbMultiSurface, ms->getGeometryType());

    // MultiSurface to MultiPolygon
    OGRGeometry* mpoly2 = OGRGeometryFactory::forceTo(ms, wkbMultiPolygon);
    ASSERT_THAT(mpoly2, NotNull());
    EXPECT_EQ(wkbMultiPolygon, mpoly2->getGeometryType());
    delete mpoly2;
  }

  // CircularString conversions
  {
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);

    // CircularString to LineString
    OGRGeometry* ls = OGRGeometryFactory::forceTo(cs.release(), wkbLineString);
    ASSERT_THAT(ls, NotNull());
    EXPECT_EQ(wkbLineString, ls->getGeometryType());
    delete ls;
  }

  // PolyhedralSurface and TIN conversions
  {
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0, 0);
    lr->addPoint(1, 0, 0);
    lr->addPoint(1, 1, 0);
    lr->addPoint(0, 0, 0);
    poly->addRingDirectly(lr.release());
    ps->addGeometryDirectly(poly.release());

    // PolyhedralSurface to MultiPolygon
    OGRGeometry* mp =
        OGRGeometryFactory::forceTo(ps.release(), wkbMultiPolygon);
    ASSERT_THAT(mp, NotNull());
    EXPECT_EQ(wkbMultiPolygon, mp->getGeometryType());

    // MultiPolygon to PolyhedralSurface
    OGRGeometry* ps2 = OGRGeometryFactory::forceTo(mp, wkbPolyhedralSurface);
    ASSERT_THAT(ps2, NotNull());
    EXPECT_EQ(wkbPolyhedralSurface, ps2->getGeometryType());

    // PolyhedralSurface to TIN
    OGRGeometry* tin = OGRGeometryFactory::forceTo(ps2, wkbTIN);
    ASSERT_THAT(tin, NotNull());
    EXPECT_EQ(wkbTIN, tin->getGeometryType());
    delete tin;
  }

  // Triangle to Polygon
  {
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());
    OGRGeometry* poly = OGRGeometryFactory::forceTo(tri.release(), wkbPolygon);
    ASSERT_THAT(poly, NotNull());
    EXPECT_EQ(wkbPolygon, poly->getGeometryType());
    delete poly;
  }
}

TEST(OgrGeometryFactoryTest, MakeCompatibleWith) {
  // Unknown target type -> unchanged
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    auto res =
        OGRGeometryFactory::makeCompatibleWith(std::move(pt), wkbUnknown);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPoint, res->getGeometryType());
  }

  // Polygon -> wkbMultiPolygon
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    auto res = OGRGeometryFactory::makeCompatibleWith(std::move(poly),
                                                      wkbMultiPolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
  }

  // Single-part MultiPolygon -> wkbPolygon
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    mp->addGeometryDirectly(poly.release());
    auto res =
        OGRGeometryFactory::makeCompatibleWith(std::move(mp), wkbPolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
  }

  // Multi-part MultiPolygon -> wkbPolygon (stays MultiPolygon)
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    for (int i = 0; i < 2; ++i) {
      auto poly = std::make_unique<OGRPolygon>();
      auto lr = std::make_unique<OGRLinearRing>();
      lr->addPoint(i * 5, 0);
      lr->addPoint(i * 5 + 1, 0);
      lr->addPoint(i * 5 + 1, 1);
      lr->addPoint(i * 5, 0);
      poly->addRingDirectly(lr.release());
      mp->addGeometryDirectly(poly.release());
    }
    auto res =
        OGRGeometryFactory::makeCompatibleWith(std::move(mp), wkbPolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
  }

  // LineString -> wkbGeometryCollection
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    auto res = OGRGeometryFactory::makeCompatibleWith(std::move(ls),
                                                      wkbGeometryCollection);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
  }

  // MultiLineString -> wkbGeometryCollection
  {
    auto mls = std::make_unique<OGRMultiLineString>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mls->addGeometryDirectly(ls.release());
    auto res = OGRGeometryFactory::makeCompatibleWith(std::move(mls),
                                                      wkbGeometryCollection);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
  }
}

TEST(OgrGeometryFactoryTest, CurveToLineStringAndCurveFromLineString) {
  // curveToLineString: standard arc
  {
    OGRLineString* ls = OGRGeometryFactory::curveToLineString(
        0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 2.0, 0.0, 0.0, false, 5.0, nullptr);
    ASSERT_THAT(ls, NotNull());
    EXPECT_GT(ls->getNumPoints(), 3);
    delete ls;
  }

  // curveToLineString with Z and ADD_INTERMEDIATE_POINT options
  {
    const char* optionsStealth[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    OGRLineString* lsStealth = OGRGeometryFactory::curveToLineString(
        0.0, 0.0, 1.0, 1.0, 1.0, 2.0, 2.0, 0.0, 3.0, true, 5.0, optionsStealth);
    ASSERT_THAT(lsStealth, NotNull());
    EXPECT_TRUE(lsStealth->Is3D());
    delete lsStealth;

    const char* optionsYes[] = {"ADD_INTERMEDIATE_POINT=YES", nullptr};
    OGRLineString* lsYes = OGRGeometryFactory::curveToLineString(
        0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 2.0, 0.0, 0.0, false, 5.0, optionsYes);
    ASSERT_THAT(lsYes, NotNull());
    delete lsYes;

    const char* optionsNo[] = {"ADD_INTERMEDIATE_POINT=NO", nullptr};
    OGRLineString* lsNo = OGRGeometryFactory::curveToLineString(
        0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 2.0, 0.0, 0.0, false, 5.0, optionsNo);
    ASSERT_THAT(lsNo, NotNull());
    delete lsNo;
  }

  // curveFromLineString: arc detection
  {
    // Generate an arc line string using curveToLineString with STEALTH
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    std::unique_ptr<OGRLineString> lsArc(OGRGeometryFactory::curveToLineString(
        0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 2.0, 0.0, 0.0, false, 5.0, options));
    ASSERT_THAT(lsArc, NotNull());

    OGRCurve* curve =
        OGRGeometryFactory::curveFromLineString(lsArc.get(), nullptr);
    ASSERT_THAT(curve, NotNull());
    EXPECT_TRUE(curve->getGeometryType() == wkbCircularString ||
                curve->getGeometryType() == wkbCompoundCurve);
    delete curve;
  }

  // curveFromLineString: closed circle
  {
    auto lsCircle = std::make_unique<OGRLineString>();
    for (int deg = 0; deg <= 360; deg += 10) {
      double rad = deg * M_PI / 180.0;
      lsCircle->addPoint(cos(rad), sin(rad));
    }
    OGRCurve* curve =
        OGRGeometryFactory::curveFromLineString(lsCircle.get(), nullptr);
    ASSERT_THAT(curve, NotNull());
    delete curve;
  }

  // curveFromLineString: straight line (non-arc)
  {
    auto lsStraight = std::make_unique<OGRLineString>();
    lsStraight->addPoint(0, 0);
    lsStraight->addPoint(1, 1);
    lsStraight->addPoint(2, 2);
    OGRCurve* curve =
        OGRGeometryFactory::curveFromLineString(lsStraight.get(), nullptr);
    ASSERT_THAT(curve, NotNull());
    EXPECT_EQ(wkbLineString, curve->getGeometryType());
    delete curve;
  }
}

TEST(OgrGeometryFactoryTest, CreateFromGeoJson) {
  // Point
  const char* geoJsonPt =
      "{\"type\": \"Point\", \"coordinates\": [10.0, 20.0]}";
  OGRGeometry* geom = OGRGeometryFactory::createFromGeoJson(geoJsonPt);
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbPoint, geom->getGeometryType());
  delete geom;

  // With explicit size
  geom = OGRGeometryFactory::createFromGeoJson(
      geoJsonPt, static_cast<int>(strlen(geoJsonPt)));
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbPoint, geom->getGeometryType());
  delete geom;

  // Polygon
  const char* geoJsonPoly =
      "{\"type\": \"Polygon\", \"coordinates\": "
      "[[[0,0],[10,0],[10,10],[0,10],[0,0]]]}";
  geom = OGRGeometryFactory::createFromGeoJson(geoJsonPoly);
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  delete geom;

  // Error cases
  EXPECT_EQ(nullptr, OGRGeometryFactory::createFromGeoJson(nullptr));
  EXPECT_EQ(nullptr, OGRGeometryFactory::createFromGeoJson(""));
  EXPECT_EQ(nullptr, OGRGeometryFactory::createFromGeoJson("{invalid json}"));

  // CPLJSONObject overload
  CPLJSONDocument doc;
  ASSERT_TRUE(doc.LoadMemory(reinterpret_cast<const GByte*>(geoJsonPt)));
  geom = OGRGeometryFactory::createFromGeoJson(doc.GetRoot());
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbPoint, geom->getGeometryType());
  delete geom;

  CPLJSONObject invalidObj;
  EXPECT_EQ(nullptr, OGRGeometryFactory::createFromGeoJson(invalidObj));
}

void AppendInt32(std::vector<unsigned char>& buf, int32_t val) {
  unsigned char b[4];
  memcpy(b, &val, 4);
  CPL_LSBPTR32(reinterpret_cast<GInt32*>(b));
  buf.insert(buf.end(), b, b + 4);
}

void AppendDouble(std::vector<unsigned char>& buf, double val) {
  unsigned char b[8];
  memcpy(b, &val, 8);
  CPL_LSBPTR64(reinterpret_cast<double*>(b));
  buf.insert(buf.end(), b, b + 8);
}

TEST(OgrGeometryFactoryTest, FgfComprehensiveDetailed) {
  // Type 0: None
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 0);
    OGRGeometry* geom = nullptr;
    int nConsumed = 0;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), &nConsumed));
    EXPECT_EQ(nullptr, geom);
    EXPECT_EQ(4, nConsumed);
  }

  // Type 1: Point 2D
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 1);
    AppendInt32(buf, 0);  // 2D
    AppendDouble(buf, 12.5);
    AppendDouble(buf, -45.0);
    OGRGeometry* geom = nullptr;
    int nConsumed = 0;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), &nConsumed));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
    EXPECT_EQ(12.5, geom->toPoint()->getX());
    EXPECT_EQ(-45.0, geom->toPoint()->getY());
    delete geom;
  }

  // Type 1: Point 3D (Z)
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 1);
    AppendInt32(buf, 1);  // 3D Z
    AppendDouble(buf, 1.0);
    AppendDouble(buf, 2.0);
    AppendDouble(buf, 3.0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_TRUE(geom->Is3D());
    delete geom;
  }

  // Type 1: Point 3D (M)
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 1);
    AppendInt32(buf, 2);  // M
    AppendDouble(buf, 1.0);
    AppendDouble(buf, 2.0);
    AppendDouble(buf, 3.0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    delete geom;
  }

  // Type 1: Point 4D (ZM)
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 1);
    AppendInt32(buf, 3);  // ZM
    AppendDouble(buf, 1.0);
    AppendDouble(buf, 2.0);
    AppendDouble(buf, 3.0);
    AppendDouble(buf, 4.0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    delete geom;
  }

  // Type 2: LineString
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 2);
    AppendInt32(buf, 0);  // 2D
    AppendInt32(buf, 2);  // 2 points
    AppendDouble(buf, 0.0);
    AppendDouble(buf, 0.0);
    AppendDouble(buf, 10.0);
    AppendDouble(buf, 20.0);
    OGRGeometry* geom = nullptr;
    int nConsumed = 0;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), &nConsumed));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    EXPECT_EQ(2, geom->toLineString()->getNumPoints());
    delete geom;
  }

  // Type 3: Polygon with 2 rings
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 3);
    AppendInt32(buf, 0);  // 2D
    AppendInt32(buf, 2);  // 2 rings
    // Ring 0 (4 points)
    AppendInt32(buf, 4);
    AppendDouble(buf, 0);
    AppendDouble(buf, 0);
    AppendDouble(buf, 10);
    AppendDouble(buf, 0);
    AppendDouble(buf, 10);
    AppendDouble(buf, 10);
    AppendDouble(buf, 0);
    AppendDouble(buf, 0);
    // Ring 1 (4 points)
    AppendInt32(buf, 4);
    AppendDouble(buf, 1);
    AppendDouble(buf, 1);
    AppendDouble(buf, 2);
    AppendDouble(buf, 1);
    AppendDouble(buf, 2);
    AppendDouble(buf, 2);
    AppendDouble(buf, 1);
    AppendDouble(buf, 1);

    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbPolygon, geom->getGeometryType());
    EXPECT_EQ(1, geom->toPolygon()->getNumInteriorRings());
    delete geom;
  }

  // Type 4: MultiPoint
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 4);
    AppendInt32(buf, 1);  // 1 point
    // Child Point
    AppendInt32(buf, 1);
    AppendInt32(buf, 0);
    AppendDouble(buf, 5.0);
    AppendDouble(buf, 6.0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
    delete geom;
  }

  // Type 5: MultiLineString
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 5);
    AppendInt32(buf, 1);  // 1 line
    // Child LineString
    AppendInt32(buf, 2);
    AppendInt32(buf, 0);
    AppendInt32(buf, 2);
    AppendDouble(buf, 0);
    AppendDouble(buf, 0);
    AppendDouble(buf, 1);
    AppendDouble(buf, 1);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbMultiLineString, geom->getGeometryType());
    delete geom;
  }

  // Type 6: MultiPolygon
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 6);
    AppendInt32(buf, 1);  // 1 polygon
    // Child Polygon
    AppendInt32(buf, 3);
    AppendInt32(buf, 0);
    AppendInt32(buf, 1);
    AppendInt32(buf, 4);
    AppendDouble(buf, 0);
    AppendDouble(buf, 0);
    AppendDouble(buf, 1);
    AppendDouble(buf, 0);
    AppendDouble(buf, 1);
    AppendDouble(buf, 1);
    AppendDouble(buf, 0);
    AppendDouble(buf, 0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
    delete geom;
  }

  // Type 7: MultiGeometry
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 7);
    AppendInt32(buf, 1);  // 1 geom
    // Child Point
    AppendInt32(buf, 1);
    AppendInt32(buf, 0);
    AppendDouble(buf, 1.0);
    AppendDouble(buf, 2.0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbGeometryCollection, geom->getGeometryType());
    delete geom;
  }

  // Recursion limit (level 32)
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    std::vector<unsigned char> buf;
    for (int i = 0; i < 33; ++i) {
      AppendInt32(buf, 7);
      AppendInt32(buf, 1);
    }
    // Child point at the end
    AppendInt32(buf, 1);
    AppendInt32(buf, 0);
    AppendDouble(buf, 0);
    AppendDouble(buf, 0);
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              OGRGeometryFactory::createFromFgf(buf.data(), nullptr, &geom,
                                                buf.size(), nullptr));
  }

  // Error cases
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRGeometry* geom = nullptr;
    // < 4 bytes
    unsigned char shortBuf[3] = {1, 0, 0};
    EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
              OGRGeometryFactory::createFromFgf(shortBuf, nullptr, &geom, 3,
                                                nullptr));

    // Unsupported type
    std::vector<unsigned char> badType;
    AppendInt32(badType, 99);
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
              OGRGeometryFactory::createFromFgf(badType.data(), nullptr, &geom,
                                                badType.size(), nullptr));

    // Corrupt dimension (> 3)
    std::vector<unsigned char> badDim;
    AppendInt32(badDim, 1);
    AppendInt32(badDim, 5);  // Dim 5
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              OGRGeometryFactory::createFromFgf(badDim.data(), nullptr, &geom,
                                                badDim.size(), nullptr));

    // Point not enough data
    std::vector<unsigned char> shortPt;
    AppendInt32(shortPt, 1);
    AppendInt32(shortPt, 0);
    AppendDouble(shortPt, 1.0);  // only 1 double instead of 2
    EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
              OGRGeometryFactory::createFromFgf(shortPt.data(), nullptr, &geom,
                                                shortPt.size(), nullptr));

    // Negative ring count
    std::vector<unsigned char> negRings;
    AppendInt32(negRings, 3);
    AppendInt32(negRings, 0);
    AppendInt32(negRings, -5);
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              OGRGeometryFactory::createFromFgf(negRings.data(), nullptr, &geom,
                                                negRings.size(), nullptr));
  }

  // C API: OGR_G_CreateFromFgf
  {
    std::vector<unsigned char> buf;
    AppendInt32(buf, 1);
    AppendInt32(buf, 0);
    AppendDouble(buf, 10.0);
    AppendDouble(buf, 20.0);
    OGRGeometryH hGeom = nullptr;
    int nConsumed = 0;
    EXPECT_EQ(OGRERR_NONE,
              OGR_G_CreateFromFgf(buf.data(), nullptr, &hGeom,
                                  static_cast<int>(buf.size()), &nConsumed));
    ASSERT_THAT(hGeom, NotNull());
    OGR_G_DestroyGeometry(hGeom);
  }
}

TEST(OgrGeometryFactoryTest, TransformWithOptionsPolarAndAntimeridianDetailed) {
  // Polar North to Geographic
  {
    OGRSpatialReference srsPolar, srsGeo;
    srsPolar.importFromEPSG(3995);
    srsGeo.importFromEPSG(4326);
    srsGeo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsPolar, &srsGeo));
    if (ct) {
      // Polygon containing North Pole
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((-100000 -100000, 100000 -100000, 100000 100000, -100000 "
          "100000, -100000 -100000))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;

      // LineString crossing pole
      OGRGeometryFactory::createFromWkt("LINESTRING (-100000 0, 100000 0)",
                                        &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      res = OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
  }

  // Polar South to Geographic
  {
    OGRSpatialReference srsPolar, srsGeo;
    srsPolar.importFromEPSG(3031);
    srsGeo.importFromEPSG(4326);
    srsGeo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsPolar, &srsGeo));
    if (ct) {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((-100000 -100000, 100000 -100000, 100000 100000, -100000 "
          "100000, -100000 -100000))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
  }

  // Antimeridian UTM zone 60N to Geographic
  {
    OGRSpatialReference srsUTM, srsGeo;
    srsUTM.importFromEPSG(32660);
    srsGeo.importFromEPSG(4326);
    srsGeo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsUTM, &srsGeo));
    if (ct) {
      // Line crossing antimeridian (Easting 829886 is 180 deg at lat 9)
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "LINESTRING (750000 996204, 900000 996204)", &srsUTM, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;

      // Polygon crossing antimeridian
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((750000 990000, 900000 990000, 900000 1000000, 750000 "
          "1000000, 750000 990000))",
          &srsUTM, &geom);
      ASSERT_THAT(geom, NotNull());
      res = OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
  }

  // Dateline crossing with WRAPDATELINE=YES and variations
  {
    const char* options[] = {"WRAPDATELINE=YES", nullptr};

    // 2D Line crossing dateline
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (176 0, -176 0)", nullptr,
                                        &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
      delete geom;
      delete res;
    }

    // 3D Line crossing dateline
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING Z (176 0 10, -176 0 20)",
                                        nullptr, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // Line with points touching 180 / -180
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (176 0, 180 0, -176 0)",
                                        nullptr, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (-176 0, -180 0, 176 0)",
                                        nullptr, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // Polygons with offset bounds
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((-250 -10, -200 -10, -200 10, -250 10, -250 -10))", nullptr,
          &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((200 -10, 250 -10, 250 10, 200 10, 200 -10))", nullptr,
          &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // Negative DATELINEOFFSET / excessive offset fallback
    {
      const char* badOptions[] = {"WRAPDATELINE=YES", "DATELINEOFFSET=-1",
                                  nullptr};
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (176 0, -176 0)", nullptr,
                                        &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, nullptr, const_cast<char**>(badOptions));
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // Warning on non-geographic CRS
    {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      OGRSpatialReference srsProj;
      srsProj.importFromEPSG(3857);
      auto pt = std::make_unique<OGRPoint>(1000, 2000);
      pt->assignSpatialReference(&srsProj);
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          pt.get(), nullptr, const_cast<char**>(options));
      ASSERT_THAT(res, NotNull());
      delete res;
    }
  }
}

TEST(OgrGeometryFactoryTest, ForceToLineStringDetailed) {
  // End0 == End1 with reversal
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto ls0 = std::make_unique<OGRLineString>();
    ls0->addPoint(0, 0);
    ls0->addPoint(5, 5);
    auto ls1 = std::make_unique<OGRLineString>();
    ls1->addPoint(10, 10);
    ls1->addPoint(5, 5);  // End0 == End1
    gc->addGeometryDirectly(ls0.release());
    gc->addGeometryDirectly(ls1.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceToLineString(gc.release(), false);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    EXPECT_EQ(3, res->toLineString()->getNumPoints());
    delete res;
  }

  // Start0 == Start1 with reversal
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto ls0 = std::make_unique<OGRLineString>();
    ls0->addPoint(0, 0);
    ls0->addPoint(5, 5);
    auto ls1 = std::make_unique<OGRLineString>();
    ls1->addPoint(0, 0);  // Start0 == Start1
    ls1->addPoint(-5, -5);
    gc->addGeometryDirectly(ls0.release());
    gc->addGeometryDirectly(ls1.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceToLineString(gc.release(), false);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    EXPECT_EQ(3, res->toLineString()->getNumPoints());
    delete res;
  }

  // End1 == Start0
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto ls0 = std::make_unique<OGRLineString>();
    ls0->addPoint(5, 5);
    ls0->addPoint(10, 10);
    auto ls1 = std::make_unique<OGRLineString>();
    ls1->addPoint(0, 0);
    ls1->addPoint(5, 5);  // End1 == Start0
    gc->addGeometryDirectly(ls0.release());
    gc->addGeometryDirectly(ls1.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceToLineString(gc.release(), true);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    EXPECT_EQ(3, res->toLineString()->getNumPoints());
    delete res;
  }

  // Disconnected lines -> returns collection of lines
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto ls0 = std::make_unique<OGRLineString>();
    ls0->addPoint(0, 0);
    ls0->addPoint(1, 1);
    auto ls1 = std::make_unique<OGRLineString>();
    ls1->addPoint(10, 10);
    ls1->addPoint(20, 20);
    gc->addGeometryDirectly(ls0.release());
    gc->addGeometryDirectly(ls1.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceToLineString(gc.release(), false);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
    delete res;
  }

  // LinearRing -> LineString
  {
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(lr.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // Polygon with no interior ring -> LineString
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(poly.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // CircularString -> LineString
  {
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(cs.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // CompoundCurve -> LineString
  {
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cc->addCurveDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToLineString(cc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    delete res;
  }

  // C API
  {
    OGRGeometryH hLS = OGR_G_CreateGeometry(wkbLineString);
    OGR_G_AddPoint_2D(hLS, 0, 0);
    OGR_G_AddPoint_2D(hLS, 1, 1);
    OGRGeometryH hRes = OGR_G_ForceToLineString(hLS);
    ASSERT_THAT(hRes, NotNull());
    OGR_G_DestroyGeometry(hRes);
  }
}

TEST(OgrGeometryFactoryTest, CurveToAndFromLineStringDetailed) {
  // GetCurveParameters
  {
    double R = 0, cx = 0, cy = 0, a0 = 0, a1 = 0, a2 = 0;
    // Collinear points -> false
    EXPECT_EQ(0, OGRGeometryFactory::GetCurveParameters(0, 0, 1, 1, 2, 2, R, cx,
                                                        cy, a0, a1, a2));
    // Coincident points -> false
    EXPECT_EQ(0, OGRGeometryFactory::GetCurveParameters(0, 0, 0, 0, 1, 1, R, cx,
                                                        cy, a0, a1, a2));
    // Valid arc
    EXPECT_EQ(1, OGRGeometryFactory::GetCurveParameters(0, 0, 1, 1, 2, 0, R, cx,
                                                        cy, a0, a1, a2));
    EXPECT_NEAR(1.0, R, 1e-6);
  }

  // GetDefaultArcStepSize
  {
    EXPECT_NEAR(4.0, OGRGeometryFactory::GetDefaultArcStepSize(), 1e-6);
    CPLSetConfigOption("OGR_ARC_STEPSIZE", "2.5");
    EXPECT_NEAR(2.5, OGRGeometryFactory::GetDefaultArcStepSize(), 1e-6);
    CPLSetConfigOption("OGR_ARC_STEPSIZE", nullptr);
  }

  // curveToLineString with STEALTH intermediate point, then curveFromLineString
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 1, 1, 0, 2, 0, 0, FALSE, 1.0, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    EXPECT_EQ(wkbCircularString, poCurve->getGeometryType());
    delete poLS;
    delete poCurve;
  }

  // curveToLineString 3D with STEALTH
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 5, 1, 1, 10, 2, 0, 15, TRUE, 1.0, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    EXPECT_EQ(wkbCircularStringZ, poCurve->getGeometryType());
    delete poLS;
    delete poCurve;
  }

  // curveToLineString with YES intermediate point
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=YES", nullptr};
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 1, 1, 0, 2, 0, 0, FALSE, 1.0, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }

  // curveToLineString with NO intermediate point
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=NO", nullptr};
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 1, 1, 0, 2, 0, 0, FALSE, 1.0, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }

  // Line with straight + arc + straight -> CompoundCurve
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    OGRLineString* poArc = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 1, 1, 0, 2, 0, 0, FALSE, 1.0, options);
    ASSERT_THAT(poArc, NotNull());

    auto fullLS = std::make_unique<OGRLineString>();
    fullLS->addPoint(-5, 0);
    fullLS->addPoint(0, 0);
    for (int i = 1; i < poArc->getNumPoints(); ++i) {
      fullLS->addPoint(poArc->getX(i), poArc->getY(i));
    }
    fullLS->addPoint(7, 0);

    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(fullLS.get());
    ASSERT_THAT(poCurve, NotNull());
    EXPECT_EQ(wkbCompoundCurve, poCurve->getGeometryType());
    delete poArc;
    delete poCurve;
  }

  // Large radius arc
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 0.5, 0.000001, 0, 1, 0, 0, FALSE, 0.01, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }

  // Straight line (no arc) -> clone
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(10, 10);
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(ls.get());
    ASSERT_THAT(poCurve, NotNull());
    EXPECT_EQ(wkbLineString, poCurve->getGeometryType());
    delete poCurve;
  }

  // approximateArcAngles with various options
  {
    // Ellipse
    OGRGeometry* geom = OGRGeometryFactory::approximateArcAngles(
        0, 0, 0, 10, 5, 45, 0, 360, 5.0, false);
    ASSERT_THAT(geom, NotNull());
    delete geom;

    // Partial arc with default step size (0.0)
    geom = OGRGeometryFactory::approximateArcAngles(0, 0, 1, 10, 10, 0, 0, 180,
                                                    0.0, true);
    ASSERT_THAT(geom, NotNull());
    delete geom;
  }
}

TEST(OgrGeometryFactoryTest, ForceToComprehensiveDetailed) {
  // Empty geometries -> createGeometry of target type
  {
    auto emptyPt = std::make_unique<OGRPoint>();
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(emptyPt.release(), wkbLineString);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbLineString, res->getGeometryType());
    EXPECT_TRUE(res->IsEmpty());
    delete res;
  }

  // PolyhedralSurface -> MultiSurface and GeometryCollection
  {
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    ps->addGeometryDirectly(poly.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(ps.release(), wkbMultiSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiSurface, res->getGeometryType());
    delete res;
  }

  // Triangle -> PolyhedralSurface, TIN, CurvePolygon, GeometryCollection
  {
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(tri.release(), wkbPolyhedralSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolyhedralSurface, res->getGeometryType());
    delete res;
  }
  {
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(tri.release(), wkbTIN);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbTIN, res->getGeometryType());
    delete res;
  }
  {
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(tri.release(), wkbCurvePolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbCurvePolygon, res->getGeometryType());
    delete res;
  }
  {
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(tri.release(), wkbGeometryCollection);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
    delete res;
  }

  // Polygon (4 points = triangle) -> Triangle and TIN
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(poly.release(), wkbTriangle);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbTriangle, res->getGeometryType());
    delete res;
  }
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(poly.release(), wkbTIN);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbTIN, res->getGeometryType());
    delete res;
  }
  {
    // Polygon with 5 points cannot convert to Triangle
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(poly.release(), wkbTriangle);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    delete res;
  }

  // MultiPolygon (all 4-point rings) -> TIN
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    mp->addGeometryDirectly(poly.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(mp.release(), wkbTIN);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbTIN, res->getGeometryType());
    delete res;
  }

  // PolyhedralSurface -> TIN
  {
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    ps->addGeometryDirectly(poly.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(ps.release(), wkbTIN);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbTIN, res->getGeometryType());
    delete res;
  }

  // TIN -> PolyhedralSurface
  {
    auto tin = std::make_unique<OGRTriangulatedSurface>();
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());
    tin->addGeometryDirectly(tri.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(tin.release(), wkbPolyhedralSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolyhedralSurface, res->getGeometryType());
    delete res;
  }

  // CurvePolygon -> CompoundCurve (steal exterior ring)
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cp->addRingDirectly(cc.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(cp.release(), wkbCompoundCurve);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbCompoundCurve, res->getGeometryType());
    delete res;
  }

  // MultiPolygon -> MultiSurface
  {
    auto mp = std::make_unique<OGRMultiPolygon>();
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mp.release(), wkbMultiSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiSurface, res->getGeometryType());
    delete res;
  }

  // MultiLineString -> MultiCurve
  {
    auto mls = std::make_unique<OGRMultiLineString>();
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mls.release(), wkbMultiCurve);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiCurve, res->getGeometryType());
    delete res;
  }

  // Target types with Z and M dimensionality
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    OGRGeometry* resZ =
        OGRGeometryFactory::forceTo(ls.release(), wkbLineString25D);
    ASSERT_THAT(resZ, NotNull());
    EXPECT_TRUE(resZ->Is3D());

    OGRGeometry* resM = OGRGeometryFactory::forceTo(resZ, wkbLineStringM);
    ASSERT_THAT(resM, NotNull());
    EXPECT_TRUE(resM->IsMeasured());

    OGRGeometry* resZM = OGRGeometryFactory::forceTo(resM, wkbLineStringZM);
    ASSERT_THAT(resZM, NotNull());
    EXPECT_TRUE(resZM->Is3D());
    EXPECT_TRUE(resZM->IsMeasured());
    delete resZM;
  }

  // C API OGR_G_ForceTo
  {
    OGRGeometryH hGeom = OGR_G_CreateGeometry(wkbPoint);
    OGR_G_AddPoint_2D(hGeom, 5, 5);
    OGRGeometryH hRes = OGR_G_ForceTo(hGeom, wkbMultiPoint, nullptr);
    ASSERT_THAT(hRes, NotNull());
    OGR_G_DestroyGeometry(hRes);
  }
}

TEST(OgrGeometryFactoryTest, OrganizePolygonsDetailed) {
  // Lake inside island inside lake inside top-level polygon
  {
    OGRGeometry* polys[3];
    // Island (CW): 0,0 to 100,100
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((0 0, 0 100, 100 100, 100 0, 0 0))", nullptr, &polys[0]);
    // Lake (CCW): 20,20 to 80,80
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((20 20, 80 20, 80 80, 20 80, 20 20))", nullptr, &polys[1]);
    // Inner island inside lake (CW): 40,40 to 60,60
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((40 40, 40 60, 60 60, 60 40, 40 40))", nullptr, &polys[2]);

    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 3, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_TRUE(bValid);
    delete res;
  }

  // Inner ring touching boundary of outer ring
  {
    OGRGeometry* polys[2];
    // Outer CW: 0,0 to 10,10
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
                                      nullptr, &polys[0]);
    // Inner CCW: touches x=0 boundary at (0, 2) to (0, 8)
    OGRGeometryFactory::createFromWkt("POLYGON ((0 2, 5 2, 5 8, 0 8, 0 2))",
                                      nullptr, &polys[1]);

    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_TRUE(bValid);
    delete res;
  }

  // METHOD=CCW_INNER_JUST_AFTER_CW_OUTER with 2 CW polygons and CCW inner rings
  {
    const char* options[] = {"METHOD=CCW_INNER_JUST_AFTER_CW_OUTER", nullptr};
    OGRGeometry* polys[3];
    // Poly 0 (CW)
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
                                      nullptr, &polys[0]);
    // Poly 1 (CCW inner of poly 0)
    OGRGeometryFactory::createFromWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
                                      nullptr, &polys[1]);
    // Poly 2 (CW 2nd outer)
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((20 20, 20 30, 30 30, 30 20, 20 20))", nullptr, &polys[2]);

    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 3, &bValid, options);
    ASSERT_THAT(res, NotNull());
    EXPECT_TRUE(bValid);
    delete res;
  }

  // CCW_INNER_JUST_AFTER_CW_OUTER fallback when 1st polygon is CCW
  {
    const char* options[] = {"METHOD=CCW_INNER_JUST_AFTER_CW_OUTER", nullptr};
    OGRGeometry* polys[2];
    // Poly 0 (CCW)
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                      nullptr, &polys[0]);
    // Poly 1 (CW)
    OGRGeometryFactory::createFromWkt("POLYGON ((20 20, 20 30, 30 30, 20 20))",
                                      nullptr, &polys[1]);

    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, options);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // Overlapping polygons with OGR_DEBUG_ORGANIZE_POLYGONS=YES
  {
    CPLSetConfigOption("OGR_DEBUG_ORGANIZE_POLYGONS", "YES");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_DEBUG_ORGANIZE_POLYGONS", nullptr); });

    OGRGeometry* polys[2];
    // Poly 0: 0,0 to 10,10
    OGRGeometryFactory::createFromWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
                                      nullptr, &polys[0]);
    // Poly 1: 5,5 to 15,15 (overlaps)
    OGRGeometryFactory::createFromWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))",
                                      nullptr, &polys[1]);

    int bValid = TRUE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    EXPECT_FALSE(bValid);
    delete res;
  }

  // Curve polygon in organizePolygons
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    cs->addPoint(5, -5);
    cs->addPoint(0, 0);
    cp->addRingDirectly(cs.release());

    OGRGeometry* polys[1] = {cp.release()};
    int bValid = FALSE;
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 1, &bValid, nullptr);
    ASSERT_THAT(res, NotNull());
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, WktAndWkbAndGeosAdditional) {
  // createFromWkt pair overload (GDAL 3.11)
  {
    auto [geom, err] =
        OGRGeometryFactory::createFromWkt("POINT (1 2)", nullptr);
    EXPECT_EQ(OGRERR_NONE, err);
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbPoint, geom->getGeometryType());
  }

  // createFromWkt with OGR_STROKE_CURVE=TRUE
  {
    CPLSetConfigOption("OGR_STROKE_CURVE", "TRUE");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_STROKE_CURVE", nullptr); });

    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "CIRCULARSTRING (0 0, 1 1, 2 0)", nullptr, &geom));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    delete geom;
  }

  // createFromWkt unknown token
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRGeometry* geom = nullptr;
    const char* badWkt = "UNKNOWNGEOM (0 0)";
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
              OGRGeometryFactory::createFromWkt(&badWkt, nullptr, &geom));
  }

  // createFromWkt empty string
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRGeometry* geom = nullptr;
    const char* emptyWkt = "";
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
              OGRGeometryFactory::createFromWkt(&emptyWkt, nullptr, &geom));
  }

  // createFromWkb corrupt byte order
  {
    unsigned char corruptByteOrder[10] = {0x42, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRGeometryFactory::createFromWkb(
                                       corruptByteOrder, nullptr, &geom, 10));
  }

  // createFromWkb with OGR_STROKE_CURVE=TRUE
  {
    CPLSetConfigOption("OGR_STROKE_CURVE", "TRUE");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_STROKE_CURVE", nullptr); });

    OGRCircularString cs;
    cs.addPoint(0, 0);
    cs.addPoint(1, 1);
    cs.addPoint(2, 0);
    std::vector<unsigned char> buf(cs.WkbSize());
    ASSERT_EQ(OGRERR_NONE, static_cast<OGRGeometry*>(&cs)->exportToWkb(
                               wkbNDR, buf.data(), wkbVariantIso));
    OGRGeometry* geom = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkb(buf.data(), nullptr, &geom,
                                                buf.size(), wkbVariantIso));
    ASSERT_THAT(geom, NotNull());
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    delete geom;
  }

#ifdef HAVE_GEOS
  // createFromGEOS with POINT EMPTY
  if (OGRGeometryFactory::haveGEOS()) {
    GEOSContextHandle_t hGEOSCtxt = OGRGeometry::createGEOSContext();
    if (hGEOSCtxt) {
      GEOSGeom geosPtEmpty = GEOSGeomFromWKT_r(hGEOSCtxt, "POINT EMPTY");
      if (geosPtEmpty) {
        OGRGeometry* geom =
            OGRGeometryFactory::createFromGEOS(hGEOSCtxt, geosPtEmpty);
        ASSERT_THAT(geom, NotNull());
        EXPECT_EQ(wkbPoint, geom->getGeometryType());
        EXPECT_TRUE(geom->IsEmpty());
        delete geom;
        GEOSGeom_destroy_r(hGEOSCtxt, geosPtEmpty);
      }
      OGRGeometry::freeGEOSContext(hGEOSCtxt);
    }
  }
#endif

  // OGR_G_CreateFromEnvelope with SRS
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRGeometryH hGeom = OGR_G_CreateFromEnvelope(
        0, 0, 10, 10, OGRSpatialReference::ToHandle(&srs));
    ASSERT_THAT(hGeom, NotNull());
    EXPECT_EQ(wkbPolygon, OGR_G_GetGeometryType(hGeom));
    OGR_G_DestroyGeometry(hGeom);
  }

  // OGR_G_CreateGeometryFromJson
  {
    const char* json = "{\"type\": \"Point\", \"coordinates\": [1.0, 2.0]}";
    OGRGeometryH hGeom = OGR_G_CreateGeometryFromJson(json);
    ASSERT_THAT(hGeom, NotNull());
    EXPECT_EQ(wkbPoint, OGR_G_GetGeometryType(hGeom));
    OGR_G_DestroyGeometry(hGeom);
  }
}

TEST(OgrGeometryFactoryTest, TransformWithOptionsAlterPoleAndPoles) {
  OGRSpatialReference srsPolar, srsGeo;
  srsPolar.importFromEPSG(3995);
  srsGeo.importFromEPSG(4326);
  srsGeo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsPolar, &srsGeo));
  if (ct) {
    // Polygon touching pole in quadrant 4 (x > 0, y < 0)
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((0 0, 100000 0, 100000 -100000, 0 -100000, 0 0))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // 3D Polygon touching pole
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON Z ((0 0 1, 100000 0 2, 100000 -100000 3, 0 -100000 4, 0 0 "
          "1))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // Vertex touching pole in middle of ring (i != 0)
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((100000 0, 100000 -100000, 0 -100000, 0 0, 100000 0))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // LineStrings touching pole
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (0 0, 100000 -100000)",
                                        &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("LINESTRING (-100000 -100000, 0 0)",
                                        &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // Donut polygon containing North Pole in its hole
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((-200000 -200000, 200000 -200000, 200000 200000, -200000 "
          "200000, -200000 -200000), "
          "(-50000 -50000, 50000 -50000, 50000 50000, -50000 50000, -50000 "
          "-50000))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }

    // MultiPolygon containing pole
    {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "MULTIPOLYGON (((-50000 -50000, 50000 -50000, 50000 50000, -50000 "
          "50000, -50000 -50000)))",
          &srsPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res =
          OGRGeometryFactory::transformWithOptions(geom, ct.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
  }

  // South Pole touching pole
  {
    OGRSpatialReference srsSouthPolar;
    srsSouthPolar.importFromEPSG(3031);
    auto ctSouth = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsSouthPolar, &srsGeo));
    if (ctSouth) {
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "POLYGON ((0 0, 100000 0, 100000 -100000, 0 -100000, 0 0))",
          &srsSouthPolar, &geom);
      ASSERT_THAT(geom, NotNull());
      OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
          geom, ctSouth.get(), nullptr);
      ASSERT_THAT(res, NotNull());
      delete geom;
      delete res;
    }
  }
}

TEST(OgrGeometryFactoryTest, DatelineExactBoundary3DAndCrossings) {
  const char* options[] = {"WRAPDATELINE=YES", nullptr};

  // (-175 0, 180 0, -174 0) in 2D and 3D
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt("LINESTRING (-175 0, 180 0, -174 0)",
                                      nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt(
        "LINESTRING Z (-175 0 1, 180 0 2, -174 0 3)", nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }

  // (175 0, -180 0, 174 0) in 2D and 3D
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt("LINESTRING (175 0, -180 0, 174 0)",
                                      nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt(
        "LINESTRING Z (175 0 1, -180 0 2, 174 0 3)", nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }

  // (-175 0, 175 0) swap in 2D and 3D
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt("LINESTRING (-175 0, 175 0)", nullptr,
                                      &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt("LINESTRING Z (-175 0 1, 175 0 2)",
                                      nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }

  // Polygons crossing in both directions with 2D and 3D
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt(
        "POLYGON ((-175 0, 175 0, 175 10, -175 10, -175 0))", nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt(
        "POLYGON Z ((-175 0 1, 175 0 2, 175 10 3, -175 10 4, -175 0 1))",
        nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }
  {
    OGRGeometry* geom = nullptr;
    OGRGeometryFactory::createFromWkt(
        "POLYGON Z ((175 0 1, -175 0 2, -175 10 3, 175 10 4, 175 0 1))",
        nullptr, &geom);
    ASSERT_THAT(geom, NotNull());
    OGRGeometry* res = OGRGeometryFactory::transformWithOptions(
        geom, nullptr, const_cast<char**>(options));
    ASSERT_THAT(res, NotNull());
    delete geom;
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, ForceToPolyhedralAndTINAndMultiSurface) {
  // forceTo(curvePoly, wkbPolyhedralSurface)
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cp->addRingDirectly(cc.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(cp.release(), wkbPolyhedralSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolyhedralSurface, res->getGeometryType());
    delete res;
  }

  // forceTo(multiSurface, wkbPolyhedralSurface)
  {
    auto ms = std::make_unique<OGRMultiSurface>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    ms->addGeometryDirectly(poly.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(ms.release(), wkbPolyhedralSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolyhedralSurface, res->getGeometryType());
    delete res;
  }

  // forceTo(curvePoly, wkbMultiSurface) and (curvePoly, wkbMultiCurve)
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cp->addRingDirectly(cc.release());

    OGRGeometry* res =
        OGRGeometryFactory::forceTo(cp.release(), wkbMultiSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiSurface, res->getGeometryType());
    delete res;
  }
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cp->addRingDirectly(cc.release());

    OGRGeometry* res = OGRGeometryFactory::forceTo(cp.release(), wkbMultiCurve);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // forceTo(geometryCollection, wkbGeometryCollection)
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRPoint(1, 2));
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(gc.release(), wkbGeometryCollection);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
    delete res;
  }

  // forceTo(multiLineString, wkbMultiCurve)
  {
    auto mls = std::make_unique<OGRMultiLineString>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mls->addGeometryDirectly(ls.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mls.release(), wkbMultiCurve);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiCurve, res->getGeometryType());
    delete res;
  }

  // forceTo with dimension modifiers on MultiLineString, MultiPolygon,
  // MultiPoint, Polygon
  {
    auto pt = std::make_unique<OGRPoint>(1, 2);
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(pt.release(), wkbMultiPointM);
    ASSERT_THAT(res, NotNull());
    EXPECT_TRUE(res->IsMeasured());
    delete res;
  }
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(poly.release(), wkbMultiPolygon25D);
    ASSERT_THAT(res, NotNull());
    EXPECT_TRUE(res->Is3D());
    delete res;
  }
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* res = OGRGeometryFactory::forceTo(poly.release(), wkbPolygonM);
    ASSERT_THAT(res, NotNull());
    EXPECT_TRUE(res->IsMeasured());
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, ForceToMultiLineStringAndPolygonComprehensive) {
  // forceToMultiPolygon(curvePoly)
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cp->addRingDirectly(cc.release());

    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(cp.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // forceToMultiPolygon(GC with curve geometry)
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(1, -1);
    cs->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cp->addRingDirectly(cc.release());
    gc->addGeometryDirectly(cp.release());

    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // forceToMultiPolygon(GC with MultiPolygon + PolyhedralSurface)
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    mp->addGeometryDirectly(poly.release());
    gc->addGeometryDirectly(mp.release());

    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto poly2 = std::make_unique<OGRPolygon>();
    auto lr2 = std::make_unique<OGRLinearRing>();
    lr2->addPoint(2, 2);
    lr2->addPoint(3, 2);
    lr2->addPoint(3, 3);
    lr2->addPoint(2, 2);
    poly2->addRingDirectly(lr2.release());
    ps->addGeometryDirectly(poly2.release());
    gc->addGeometryDirectly(ps.release());

    OGRGeometry* res = OGRGeometryFactory::forceToMultiPolygon(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // forceToMultiLineString(GC with curve geometry)
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    gc->addGeometryDirectly(cs.release());

    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }

  // forceToMultiLineString(circularString)
  {
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(cs.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }

  // forceToMultiLineString(compoundCurve)
  {
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cc->addCurveDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(cc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }

  // forceToMultiLineString(multiCurve without curves and with curves)
  {
    auto mc = std::make_unique<OGRMultiCurve>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mc->addGeometryDirectly(ls.release());
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(mc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }
  {
    auto mc = std::make_unique<OGRMultiCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    mc->addGeometryDirectly(cs.release());
    OGRGeometry* res = OGRGeometryFactory::forceToMultiLineString(mc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }

  // forceToPolygon(polyhedralSurface with 1 polygon)
  {
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(1, 0);
    lr->addPoint(1, 1);
    lr->addPoint(0, 0);
    poly->addRingDirectly(lr.release());
    ps->addGeometryDirectly(poly.release());

    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(ps.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    delete res;
  }

  // forceToPolygon(GC with multiple polygons with interior rings)
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto poly1 = std::make_unique<OGRPolygon>();
    auto lr1 = std::make_unique<OGRLinearRing>();
    lr1->addPoint(0, 0);
    lr1->addPoint(10, 0);
    lr1->addPoint(10, 10);
    lr1->addPoint(0, 0);
    poly1->addRingDirectly(lr1.release());
    auto ringInner1 = std::make_unique<OGRLinearRing>();
    ringInner1->addPoint(1, 1);
    ringInner1->addPoint(2, 1);
    ringInner1->addPoint(2, 2);
    ringInner1->addPoint(1, 1);
    poly1->addRingDirectly(ringInner1.release());
    gc->addGeometryDirectly(poly1.release());

    auto poly2 = std::make_unique<OGRPolygon>();
    auto lr2 = std::make_unique<OGRLinearRing>();
    lr2->addPoint(20, 20);
    lr2->addPoint(30, 20);
    lr2->addPoint(30, 30);
    lr2->addPoint(20, 20);
    poly2->addRingDirectly(lr2.release());
    gc->addGeometryDirectly(poly2.release());

    OGRGeometry* res = OGRGeometryFactory::forceToPolygon(gc.release());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    delete res;
  }

  // removeLowerDimensionSubGeoms: 1D without curves -> MultiLineString
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto ls1 = std::make_unique<OGRLineString>();
    ls1->addPoint(0, 0);
    ls1->addPoint(1, 1);
    auto ls2 = std::make_unique<OGRLineString>();
    ls2->addPoint(2, 2);
    ls2->addPoint(3, 3);
    gc->addGeometryDirectly(ls1.release());
    gc->addGeometryDirectly(ls2.release());
    gc->addGeometryDirectly(new OGRPoint(5, 5));

    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }
}

TEST(OgrGeometryFactoryTest, DetectArcCollinearAndLargeRadius) {
  // Collinear points in curveToLineString
  {
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 1, 1, 0, 2, 2, 0, FALSE, 1.0, nullptr);
    ASSERT_THAT(poLS, NotNull());
    delete poLS;
  }

  // Large radius arc (nSearchRadius = 10)
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    // Center at (5, -100), R = sqrt(25 + 10000) = 100.1249
    double R = sqrt(25.0 + 10000.0);
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 5, -100.0 + R, 0, 10, 0, 0, FALSE, 0.5, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }

  // Very large radius arc (nSearchRadius = 100)
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    double R = sqrt(25.0 + 100000000.0);
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 5, -10000.0 + R, 0, 10, 0, 0, FALSE, 0.05, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }
}

TEST(OgrGeometryFactoryTest, FinalCoverageBoost) {
  // 1. forceTo GeometryCollection conversions (lines 4820-4828)
  {
    auto mp = std::make_unique<OGRMultiPoint>();
    mp->addGeometryDirectly(new OGRPoint(1, 2));
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mp.release(), wkbGeometryCollection);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
    delete res;
  }
  {
    auto mls = std::make_unique<OGRMultiLineString>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mls->addGeometryDirectly(ls.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mls.release(), wkbGeometryCollection);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection, res->getGeometryType());
    delete res;
  }
  {
    auto mp = std::make_unique<OGRMultiPoint>();
    mp->addGeometryDirectly(new OGRPoint(1, 2, 3));
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mp.release(), wkbGeometryCollection25D);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollection25D, res->getGeometryType());
    delete res;
  }
  {
    auto mp = std::make_unique<OGRMultiPoint>();
    auto pt = new OGRPoint(1, 2);
    pt->setM(5);
    mp->addGeometryDirectly(pt);
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(mp.release(), wkbGeometryCollectionM);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbGeometryCollectionM, res->getGeometryType());
    delete res;
  }

  // 2. forceTo closed LineString to MultiSurface and MultiPolygon
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(0, 10);
    ls->addPoint(10, 10);
    ls->addPoint(10, 0);
    ls->addPoint(0, 0);
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(ls.release(), wkbMultiSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiSurface, wkbFlatten(res->getGeometryType()));
    delete res;
  }
  {
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(10, 0);
    ls->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cc->addCurveDirectly(ls.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(cc.release(), wkbMultiSurface);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiSurface, wkbFlatten(res->getGeometryType()));
    delete res;
  }
  {
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(10, 0);
    ls->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cc->addCurveDirectly(ls.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(cc.release(), wkbMultiPolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, wkbFlatten(res->getGeometryType()));
    delete res;
  }

  // 3. forceTo closed curve to CurvePolygon
  {
    auto cc = std::make_unique<OGRCompoundCurve>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(10, 0);
    ls->addPoint(0, 0);
    cc->addCurveDirectly(cs.release());
    cc->addCurveDirectly(ls.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(cc.release(), wkbCurvePolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbCurvePolygon, wkbFlatten(res->getGeometryType()));
    delete res;
  }

  // 4. forceTo triangle to CurvePolygon
  {
    auto tri = std::make_unique<OGRTriangle>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(0, 10);
    lr->addPoint(10, 0);
    lr->addPoint(0, 0);
    tri->addRingDirectly(lr.release());
    OGRGeometry* res =
        OGRGeometryFactory::forceTo(tri.release(), wkbCurvePolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbCurvePolygon, wkbFlatten(res->getGeometryType()));
    delete res;
  }

  // 5. forceTo GC with single Point to Polygon
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRPoint(1, 1));
    OGRGeometry* res = OGRGeometryFactory::forceTo(gc.release(), wkbPolygon);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolygon, res->getGeometryType());
    delete res;
  }

  // 6. forceToPolygon with closed CircularString
  {
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    cs->addPoint(5, -5);
    cs->addPoint(0, 0);
    OGRGeometry* poly = OGRGeometryFactory::forceToPolygon(cs.release());
    ASSERT_THAT(poly, NotNull());
    EXPECT_EQ(wkbPolygon, poly->getGeometryType());
    delete poly;
  }

  // 7. forceToMultiLineString with CurvePolygon, MultiSurface,
  // PolyhedralSurface
  {
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    cs->addPoint(5, -5);
    cs->addPoint(0, 0);
    cp->addRingDirectly(cs.release());
    OGRGeometry* mls = OGRGeometryFactory::forceToMultiLineString(cp.release());
    ASSERT_THAT(mls, NotNull());
    EXPECT_EQ(wkbMultiLineString, mls->getGeometryType());
    delete mls;
  }
  {
    auto ms = std::make_unique<OGRMultiSurface>();
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(5, 5);
    cs->addPoint(10, 0);
    cs->addPoint(5, -5);
    cs->addPoint(0, 0);
    cp->addRingDirectly(cs.release());
    ms->addGeometryDirectly(cp.release());
    OGRGeometry* mls = OGRGeometryFactory::forceToMultiLineString(ms.release());
    ASSERT_THAT(mls, NotNull());
    EXPECT_EQ(wkbMultiLineString, mls->getGeometryType());
    delete mls;
  }
  {
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto p = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(0, 10);
    lr->addPoint(10, 10);
    lr->addPoint(10, 0);
    lr->addPoint(0, 0);
    p->addRingDirectly(lr.release());
    ps->addGeometryDirectly(p.release());
    OGRGeometry* mls = OGRGeometryFactory::forceToMultiLineString(ps.release());
    ASSERT_THAT(mls, NotNull());
    EXPECT_EQ(wkbMultiLineString, mls->getGeometryType());
    delete mls;
  }

  // 8. removeLowerDimensionSubGeoms with curves and nested GC
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto cs1 = std::make_unique<OGRCircularString>();
    cs1->addPoint(0, 0);
    cs1->addPoint(1, 1);
    cs1->addPoint(2, 0);
    auto cs2 = std::make_unique<OGRCircularString>();
    cs2->addPoint(2, 0);
    cs2->addPoint(3, 1);
    cs2->addPoint(4, 0);
    gc->addGeometryDirectly(cs1.release());
    gc->addGeometryDirectly(cs2.release());
    gc->addGeometryDirectly(new OGRPoint(5, 5));
    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiCurve, res->getGeometryType());
    delete res;
  }
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto mls = std::make_unique<OGRMultiLineString>();
    auto ls1 = std::make_unique<OGRLineString>();
    ls1->addPoint(0, 0);
    ls1->addPoint(1, 1);
    mls->addGeometryDirectly(ls1.release());
    gc->addGeometryDirectly(mls.release());
    auto ls2 = std::make_unique<OGRLineString>();
    ls2->addPoint(2, 2);
    ls2->addPoint(3, 3);
    gc->addGeometryDirectly(ls2.release());
    gc->addGeometryDirectly(new OGRPoint(0, 0));
    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiLineString, res->getGeometryType());
    delete res;
  }
  {
    auto gc = std::make_unique<OGRGeometryCollection>();
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto p1 = std::make_unique<OGRPolygon>();
    auto lr1 = std::make_unique<OGRLinearRing>();
    lr1->addPoint(0, 0);
    lr1->addPoint(0, 1);
    lr1->addPoint(1, 1);
    lr1->addPoint(1, 0);
    lr1->addPoint(0, 0);
    p1->addRingDirectly(lr1.release());
    mp->addGeometryDirectly(p1.release());
    gc->addGeometryDirectly(mp.release());
    auto p2 = std::make_unique<OGRPolygon>();
    auto lr2 = std::make_unique<OGRLinearRing>();
    lr2->addPoint(2, 2);
    lr2->addPoint(2, 3);
    lr2->addPoint(3, 3);
    lr2->addPoint(3, 2);
    lr2->addPoint(2, 2);
    p2->addRingDirectly(lr2.release());
    gc->addGeometryDirectly(p2.release());
    gc->addGeometryDirectly(new OGRPoint(0, 0));
    OGRGeometry* res =
        OGRGeometryFactory::removeLowerDimensionSubGeoms(gc.get());
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbMultiPolygon, res->getGeometryType());
    delete res;
  }

  // 9. organizePolygons CCW_INNER_JUST_AFTER_CW_OUTER fallback and ONLY_CCW
  {
    auto p1 = std::make_unique<OGRPolygon>();
    auto lr1 = std::make_unique<OGRLinearRing>();
    lr1->addPoint(0, 0);
    lr1->addPoint(100, 0);
    lr1->addPoint(100, 100);
    lr1->addPoint(0, 100);
    lr1->addPoint(0, 0);
    p1->addRingDirectly(lr1.release());

    auto p2 = std::make_unique<OGRPolygon>();
    auto lr2 = std::make_unique<OGRLinearRing>();
    lr2->addPoint(1, 1);
    lr2->addPoint(1, 10);
    lr2->addPoint(10, 10);
    lr2->addPoint(10, 1);
    lr2->addPoint(1, 1);
    p2->addRingDirectly(lr2.release());

    auto p3 = std::make_unique<OGRPolygon>();
    auto lr3 = std::make_unique<OGRLinearRing>();
    lr3->addPoint(20, 20);
    lr3->addPoint(20, 30);
    lr3->addPoint(30, 30);
    lr3->addPoint(30, 20);
    lr3->addPoint(20, 20);
    p3->addRingDirectly(lr3.release());

    OGRGeometry* polys[3] = {p1.release(), p2.release(), p3.release()};
    const char* options[] = {"METHOD=CCW_INNER_JUST_AFTER_CW_OUTER", nullptr};
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 3, nullptr, options);
    ASSERT_THAT(res, NotNull());
    delete res;
  }
  {
    auto p1 = std::make_unique<OGRPolygon>();
    auto lr1 = std::make_unique<OGRLinearRing>();
    lr1->addPoint(0, 0);
    lr1->addPoint(0, 100);
    lr1->addPoint(100, 100);
    lr1->addPoint(100, 0);
    lr1->addPoint(0, 0);
    p1->addRingDirectly(lr1.release());

    auto p2 = std::make_unique<OGRPolygon>();
    auto lr2 = std::make_unique<OGRLinearRing>();
    lr2->addPoint(1, 1);
    lr2->addPoint(1, 10);
    lr2->addPoint(10, 10);
    lr2->addPoint(10, 1);
    lr2->addPoint(1, 1);
    p2->addRingDirectly(lr2.release());

    OGRGeometry* polys[2] = {p1.release(), p2.release()};
    const char* options[] = {"METHOD=ONLY_CCW", nullptr};
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, nullptr, options);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // 10. organizePolygons vertices all on boundary
  {
    auto pj = std::make_unique<OGRPolygon>();
    auto lr_j = std::make_unique<OGRLinearRing>();
    lr_j->addPoint(0, 0);
    lr_j->addPoint(10, 0);
    lr_j->addPoint(10, 10);
    lr_j->addPoint(0, 10);
    lr_j->addPoint(0, 0);
    pj->addRingDirectly(lr_j.release());

    auto pi = std::make_unique<OGRPolygon>();
    auto lr_i = std::make_unique<OGRLinearRing>();
    lr_i->addPoint(0, 2);
    lr_i->addPoint(0, 8);
    lr_i->addPoint(5, 10);
    lr_i->addPoint(0, 2);
    pi->addRingDirectly(lr_i.release());

    OGRGeometry* polys[2] = {pj.release(), pi.release()};
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, nullptr, nullptr);
    ASSERT_THAT(res, NotNull());
    delete res;
  }
  {
    auto pj = std::make_unique<OGRPolygon>();
    auto lr_j = std::make_unique<OGRLinearRing>();
    lr_j->addPoint(0, 0);
    lr_j->addPoint(10, 0);
    lr_j->addPoint(10, 10);
    lr_j->addPoint(5, 5);
    lr_j->addPoint(0, 10);
    lr_j->addPoint(0, 0);
    pj->addRingDirectly(lr_j.release());

    auto pi = std::make_unique<OGRPolygon>();
    auto lr_i = std::make_unique<OGRLinearRing>();
    lr_i->addPoint(0, 10);
    lr_i->addPoint(10, 10);
    lr_i->addPoint(5, 5);
    lr_i->addPoint(0, 10);
    pi->addRingDirectly(lr_i.release());

    OGRGeometry* polys[2] = {pj.release(), pi.release()};
    OGRGeometry* res =
        OGRGeometryFactory::organizePolygons(polys, 2, nullptr, nullptr);
    ASSERT_THAT(res, NotNull());
    delete res;
  }

  // 11. approximateArcAngles with dfMaxInterpolationGap
  {
    CPLSetConfigOption("OGR_ARC_MAX_GAP", "0.1");
    OGRGeometry* poLS = OGRGeometryFactory::approximateArcAngles(
        0.0, 0.0, 0.0, 10.0, 5.0, 0.0, 0.0, 90.0, 45.0, true);
    CPLSetConfigOption("OGR_ARC_MAX_GAP", nullptr);
    ASSERT_THAT(poLS, NotNull());
    delete poLS;
  }

  // 12. forceTo with dimensional flags
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    OGRGeometry* res1 =
        OGRGeometryFactory::forceTo(ls.release(), wkbMultiLineString25D);
    ASSERT_THAT(res1, NotNull());
    EXPECT_EQ(wkbMultiLineString25D, res1->getGeometryType());
    delete res1;

    auto ls2 = std::make_unique<OGRLineString>();
    ls2->addPoint(0, 0);
    ls2->addPoint(1, 1);
    OGRGeometry* res2 =
        OGRGeometryFactory::forceTo(ls2.release(), wkbMultiLineStringM);
    ASSERT_THAT(res2, NotNull());
    EXPECT_EQ(wkbMultiLineStringM, res2->getGeometryType());
    delete res2;

    auto pt = std::make_unique<OGRPoint>(1, 1);
    OGRGeometry* res3 =
        OGRGeometryFactory::forceTo(pt.release(), wkbMultiPoint25D);
    ASSERT_THAT(res3, NotNull());
    EXPECT_EQ(wkbMultiPoint25D, res3->getGeometryType());
    delete res3;

    auto pt2 = std::make_unique<OGRPoint>(1, 1);
    OGRGeometry* res4 =
        OGRGeometryFactory::forceTo(pt2.release(), wkbMultiPointM);
    ASSERT_THAT(res4, NotNull());
    EXPECT_EQ(wkbMultiPointM, res4->getGeometryType());
    delete res4;
  }

  // 13. forceTo PolyhedralSurface to wkbTIN
  {
    // Non-triangle polygon (5 points) -> returns original geometry
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto p = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(0, 10);
    lr->addPoint(10, 10);
    lr->addPoint(10, 0);
    lr->addPoint(0, 0);
    p->addRingDirectly(lr.release());
    ps->addGeometryDirectly(p.release());
    OGRGeometry* res = OGRGeometryFactory::forceTo(ps.release(), wkbTIN);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbPolyhedralSurface, res->getGeometryType());
    delete res;
  }
  {
    // Triangle polygon (4 points) -> converts to TIN (OGRTriangulatedSurface)
    auto ps = std::make_unique<OGRPolyhedralSurface>();
    auto p = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(0, 0);
    lr->addPoint(0, 10);
    lr->addPoint(10, 0);
    lr->addPoint(0, 0);
    p->addRingDirectly(lr.release());
    ps->addGeometryDirectly(p.release());
    OGRGeometry* res = OGRGeometryFactory::forceTo(ps.release(), wkbTIN);
    ASSERT_THAT(res, NotNull());
    EXPECT_EQ(wkbTIN, res->getGeometryType());
    delete res;
  }

  // 14. GetCurveParameters full circle
  {
    double R = 0, cx = 0, cy = 0, a0 = 0, a1 = 0, a2 = 0;
    int bArc = OGRGeometryFactory::GetCurveParameters(0, 0, 10, 0, 0, 0, R, cx,
                                                      cy, a0, a1, a2);
    EXPECT_TRUE(bArc);
    EXPECT_DOUBLE_EQ(5.0, R);

    bArc = OGRGeometryFactory::GetCurveParameters(0, 0, 0, 0, 0, 0, R, cx, cy,
                                                  a0, a1, a2);
    EXPECT_FALSE(bArc);
  }

  // 15. Full circle non-polygonal curveFromLineString
  {
    const char* options[] = {"ADD_INTERMEDIATE_POINT=STEALTH", nullptr};
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 5, 10, 0, 5, 0, 0, 5, FALSE, 5.0, options);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }
  {
    OGRLineString* poLS = OGRGeometryFactory::curveToLineString(
        0, 0, 0, 10, 0, 0, 0, 0, 0, FALSE, 5.0, nullptr);
    ASSERT_THAT(poLS, NotNull());
    OGRCurve* poCurve = OGRGeometryFactory::curveFromLineString(poLS);
    ASSERT_THAT(poCurve, NotNull());
    delete poLS;
    delete poCurve;
  }

  // 16. transformWithOptions Dateline tests
  {
    const char* options[] = {"WRAPDATELINE=YES", nullptr};

    auto mp = std::make_unique<OGRMultiPoint>();
    mp->addGeometryDirectly(new OGRPoint(-185, 0));
    mp->addGeometryDirectly(new OGRPoint(185, 0));
    OGRGeometry* resMP = OGRGeometryFactory::transformWithOptions(
        mp.get(), nullptr, const_cast<char**>(options));
    ASSERT_THAT(resMP, NotNull());
    delete resMP;

    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRPoint(-185, 0));
    auto ls3d = std::make_unique<OGRLineString>();
    ls3d->addPoint(-175, 0, 10);
    ls3d->addPoint(175, 0, 20);
    gc->addGeometryDirectly(ls3d.release());
    OGRGeometry* resGC = OGRGeometryFactory::transformWithOptions(
        gc.get(), nullptr, const_cast<char**>(options));
    ASSERT_THAT(resGC, NotNull());
    delete resGC;

    auto poly = std::make_unique<OGRPolygon>();
    auto lr = std::make_unique<OGRLinearRing>();
    lr->addPoint(-176, 0);
    lr->addPoint(-175, 5);
    lr->addPoint(175, 5);
    lr->addPoint(175, 0);
    lr->addPoint(-176, 0);
    poly->addRingDirectly(lr.release());
    OGRGeometry* resPoly = OGRGeometryFactory::transformWithOptions(
        poly.get(), nullptr, const_cast<char**>(options));
    ASSERT_THAT(resPoly, NotNull());
    delete resPoly;
  }

  // 17. Antimeridian with interior rings and GeometryCollection
  {
    OGRSpatialReference srsNorthPole;
    srsNorthPole.importFromEPSG(3995);
    OGRSpatialReference srsGeo;
    srsGeo.importFromEPSG(4326);
    srsGeo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNorthPole, &srsGeo));

    if (ct != nullptr) {
      const char* options[] = {"WRAPDATELINE=YES", nullptr};

      auto poly = std::make_unique<OGRPolygon>();
      auto ext = std::make_unique<OGRLinearRing>();
      ext->addPoint(-100000, -100000);
      ext->addPoint(-100000, 100000);
      ext->addPoint(100000, 100000);
      ext->addPoint(100000, -100000);
      ext->addPoint(-100000, -100000);
      poly->addRingDirectly(ext.release());

      auto inRing = std::make_unique<OGRLinearRing>();
      inRing->addPoint(-50000, -50000);
      inRing->addPoint(-50000, 50000);
      inRing->addPoint(50000, 50000);
      inRing->addPoint(50000, -50000);
      inRing->addPoint(-50000, -50000);
      poly->addRingDirectly(inRing.release());

      OGRGeometry* resPoly = OGRGeometryFactory::transformWithOptions(
          poly.get(), ct.get(), const_cast<char**>(options));
      if (resPoly) delete resPoly;

      auto mp = std::make_unique<OGRMultiPolygon>();
      auto p1 = std::make_unique<OGRPolygon>();
      auto r1 = std::make_unique<OGRLinearRing>();
      r1->addPoint(-10000, -10000);
      r1->addPoint(-10000, 10000);
      r1->addPoint(10000, 10000);
      r1->addPoint(10000, -10000);
      r1->addPoint(-10000, -10000);
      p1->addRingDirectly(r1.release());
      mp->addGeometryDirectly(p1.release());

      OGRGeometry* resMP = OGRGeometryFactory::transformWithOptions(
          mp.get(), ct.get(), const_cast<char**>(options));
      if (resMP) delete resMP;
    }
  }
}

}  // namespace
}  // namespace autotest2
