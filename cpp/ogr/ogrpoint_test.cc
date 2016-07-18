// Tests ogr/ogrpoint.cpp.
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <memory>

#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_geometry.h"

using autotest2::FreeDeleter;

namespace {

// The getDimension() method is refering points (as in an x, y, z
// location) having zero dimensions.  A line is 1D, a polygon is
// 2D. It does not refer to how many coordinates are set in the point
// (x/y versus x/y/z).

TEST(OgrPointTest, DefaultConstructor) {
  // Tests starting from a default constructor.
  OGRPoint point;
  EXPECT_EQ(0, point.getDimension());
  EXPECT_TRUE(point.IsEmpty());
  EXPECT_DOUBLE_EQ(0.0, point.getX());
  EXPECT_DOUBLE_EQ(0.0, point.getY());
  EXPECT_DOUBLE_EQ(0.0, point.getZ());

  EXPECT_STREQ("POINT", point.getGeometryName());
  EXPECT_EQ(wkbPoint, point.getGeometryType());

  // Do not compare with nullptr as that will crash.
  EXPECT_TRUE(point.Equals(&point));

  OGRPoint point2;
  EXPECT_TRUE(point.Equals(&point2));  // Uses an int for boolean return.
  point2.setX(123.0);
  EXPECT_FALSE(point.Equals(&point2));
  point2.empty();
  EXPECT_TRUE(point.Equals(&point2));
  EXPECT_EQ(0, point2.getDimension());

  point.setX(123.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_DOUBLE_EQ(123.0, point.getX());
  point.setY(456.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_DOUBLE_EQ(456.0, point.getY());
  point.setZ(789.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_DOUBLE_EQ(789.0, point.getZ());
}

TEST(OgrPointTest, ConstructWithTwoCoords) {
  // Tests that starting from a constructor with X and Y is sane.
  OGRPoint point(-1.0, -2.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_FALSE(point.IsEmpty());
  EXPECT_DOUBLE_EQ(-1.0, point.getX());
  EXPECT_DOUBLE_EQ(-2.0, point.getY());
  EXPECT_DOUBLE_EQ(0.0, point.getZ());

  EXPECT_STREQ("POINT", point.getGeometryName());
  EXPECT_EQ(wkbPoint, point.getGeometryType());

  // Do not compare with nullptr as that will crash.
  EXPECT_TRUE(point.Equals(&point));

  OGRPoint point2(-1.0, -2.0);
  EXPECT_TRUE(point.Equals(&point2));  // Uses an int for boolean return.
  point2.setX(123.0);
  EXPECT_FALSE(point.Equals(&point2));
  point2.empty();
  EXPECT_FALSE(point.Equals(&point2));
  EXPECT_EQ(0, point2.getDimension());

  point.setX(123.0);
  EXPECT_DOUBLE_EQ(123.0, point.getX());
  point.setY(456.0);
  EXPECT_DOUBLE_EQ(456.0, point.getY());
  point.setZ(789.0);
  EXPECT_DOUBLE_EQ(789.0, point.getZ());

  point.swapXY();
  EXPECT_DOUBLE_EQ(456.0, point.getX());
  EXPECT_DOUBLE_EQ(123.0, point.getY());
  EXPECT_DOUBLE_EQ(789.0, point.getZ());
}

TEST(OgrPointTest, ConstructWithThreeCoords) {
  // Tests that starting from a constructor with X, Y, and Z is sane.
  OGRPoint point(-3.0, -4.0, 5.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_FALSE(point.IsEmpty());
  EXPECT_DOUBLE_EQ(-3.0, point.getX());
  EXPECT_DOUBLE_EQ(-4.0, point.getY());
  EXPECT_DOUBLE_EQ(5.0, point.getZ());

  EXPECT_STREQ("POINT", point.getGeometryName());
  EXPECT_EQ(wkbPoint25D, point.getGeometryType());

  // Do not compare with nullptr as that will crash.
  EXPECT_TRUE(point.Equals(&point));

  OGRPoint point2(-3.0, -4.0, 5.0);
  EXPECT_TRUE(point.Equals(&point2));  // Uses an int for boolean return.
  point2.setZ(6.0);
  EXPECT_FALSE(point.Equals(&point2));

  point.setX(123.0);
  EXPECT_DOUBLE_EQ(123.0, point.getX());
  point.setY(456.0);
  EXPECT_DOUBLE_EQ(456.0, point.getY());
  point.setZ(789.0);
  EXPECT_DOUBLE_EQ(789.0, point.getZ());

  point.swapXY();
  EXPECT_DOUBLE_EQ(456.0, point.getX());
  EXPECT_DOUBLE_EQ(123.0, point.getY());
  EXPECT_DOUBLE_EQ(789.0, point.getZ());
}

TEST(OgrPointTest, WktImportEmptyString) {
  OGRPoint point;
  std::unique_ptr<char, FreeDeleter> empty_str(strdup(""));
  char *empty_ptr = empty_str.get();
  EXPECT_EQ(OGRERR_CORRUPT_DATA, point.importFromWkt(&empty_ptr));
  OGRPoint point2;
  EXPECT_TRUE(point.Equals(&point2));
}

TEST(OgrPointTest, WktImportEmpty) {
  OGRPoint point;
  std::unique_ptr<char, FreeDeleter> empty_str(strdup("POINT EMPTY"));
  char *empty_ptr = empty_str.get();
  EXPECT_EQ(OGRERR_NONE, point.importFromWkt(&empty_ptr));
  OGRPoint point2;
  EXPECT_TRUE(point.Equals(&point2));
}

TEST(OgrPointTest, WktImportPoint) {
  OGRPoint point;
  std::unique_ptr<char, FreeDeleter> wkt(strdup("POINT (2 3)"));
  char *wkt_ptr = wkt.get();
  EXPECT_EQ(OGRERR_NONE, point.importFromWkt(&wkt_ptr));
  OGRPoint point2(2.0, 3.0);
  EXPECT_TRUE(point.Equals(&point2));
}

TEST(OgrPointTest, WktExport) {
  OGRPoint point;

  char *wkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT EMPTY", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setX(1);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (1 0)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  // Try using a float rather than an int.
  point.setY(2.0);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (1 2)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setZ(3);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (1 2 3)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.empty();
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT EMPTY", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setX(-1);
  point.setY(-1.2);
  point.setZ(-1.3);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (-1.0 -1.2 -1.3)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setZ(1.3);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (-1.0 -1.2 1.3)", wkt);
  CPLFree(wkt);
}

// TODO(schwehr): Write more tests.
