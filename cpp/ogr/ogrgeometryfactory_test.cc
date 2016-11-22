// Tests ogrgeometry_factory.cpp.
//
// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
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
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_wkbwkt_geom.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_wktempty.py

#include "port/cpl_port.h"

#include <memory>
#include <string>

#include "ogr/ogr_core.h"
#include "ogr/ogr_geometry.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "gunit.h"

namespace {

using std::unique_ptr;

unique_ptr<OGRGeometry> GeomFromWkt(const char *wkt) {
  CHECK_NOTNULL(wkt);
  OGRGeometry *geom = nullptr;
  unique_ptr<char, autotest2::FreeDeleter> wkt_copy(strdup(wkt));
  char *wkt_end = wkt_copy.get();
  EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(&wkt_end, nullptr,
                                                           &geom));
  // Updated to point to the just past last character consumed.
  EXPECT_EQ('\0', *wkt_end);

  return unique_ptr<OGRGeometry>(geom);
}

unique_ptr<OGRGeometry> GeomFromWkb(const unsigned char *wkb, int length) {
  CHECK_NOTNULL(wkb);
  OGRGeometry *geom = nullptr;
  EXPECT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkb(
                const_cast<unsigned char *>(wkb), nullptr, &geom,
                length));

  return unique_ptr<OGRGeometry>(geom);
}

string Wkt(OGRGeometry &geom) {
  char *wkt_tmp = nullptr;
  EXPECT_EQ(OGRERR_NONE, geom.exportToWkt(&wkt_tmp));
  CHECK_NOTNULL(wkt_tmp);
  string wkt(wkt_tmp);
  free(wkt_tmp);

  return wkt;
}

TEST(OgrGemeometryFactoryWkbWktTest, MultiPoint) {
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

TEST(OgrGemeometryFactoryWkbWktTest, MultiLineString) {
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

  const string wkt_from_wkt = Wkt(*geom_from_wkt);
  const string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGemeometryFactoryWkbWktTest, GeometryCollection) {
  auto geom_from_wkt = GeomFromWkt(
      "GEOMETRYCOLLECTION("
      "POINT(5.0012134894 9.998531313699999),"
      "POINT (5.0012134888 7.9985313168),"
      "LINESTRING (3.0012135082 8.9985312634,""4.0012134989 9.998531287600001,"
      "5.0012134891 8.998531314699999,4.0012134982 7.9985312908),"
      "POLYGON ((4.0012134982 7.9985312908,""4.0012134986 8.998531289300001,"
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

  const string wkt_from_wkt = Wkt(*geom_from_wkt);
  const string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGemeometryFactoryWkbWktTest, MultiPolygon) {
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

  const string wkt_from_wkt = Wkt(*geom_from_wkt);
  const string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGemeometryFactoryWkbWktTest, LineString) {
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

  const string wkt_from_wkt = Wkt(*geom_from_wkt);
  const string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

TEST(OgrGemeometryFactoryWkbWktTest, Polygon) {
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

  const string wkt_from_wkt = Wkt(*geom_from_wkt);
  const string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

// TODO(schwehr): What was different about the polygons in 6.wkt and 7.wkt?

// The upstream 8.wkt POINT has unbalanced parens.  Why?
// https://trac.osgeo.org/gdal/ticket/6725

TEST(OgrGemeometryFactoryWkbWktTest, Point) {
  auto geom_from_wkt = GeomFromWkt("POINT (2.0012135167 5.9985312409)");
  EXPECT_EQ(wkbPoint, geom_from_wkt->getGeometryType());

  const unsigned char kWkb[] =
      "\x00\x00\x00\x00\x01\x40\x00\x02\x7c\x3b\x74\x51\x34\x40\x17\xfe\x7e"
      "\xf9\x3e\xcc\x98";

  auto geom_from_wkb = GeomFromWkb(kWkb, CPL_ARRAYSIZE(kWkb));

  const string wkt_from_wkt = Wkt(*geom_from_wkt);
  const string wkt_from_wkb = Wkt(*geom_from_wkb);
  EXPECT_EQ(wkt_from_wkt, wkt_from_wkb);
}

// TODO(schwehr): What is the reason for having 8.wkt, 9.wkt and 10.wkt?
// All POINTS, but what is special?
// And 11.wkt is another MULTIPOINT.  What is different from 1.wkt?

}  // namespace
