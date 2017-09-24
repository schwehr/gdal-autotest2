// Tests ogrspatialreference.cpp.
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

#include <string>

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace {

TEST(OgrSpatialReferenceTest, NullConstructor) {
  OGRSpatialReference srs(nullptr);
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_EQ(nullptr, srs.GetRoot());
  srs.Clear();
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_EQ(nullptr, srs.GetRoot());
  EXPECT_FALSE(srs.IsGeographic());
  EXPECT_FALSE(srs.IsProjected());
  EXPECT_FALSE(srs.IsGeocentric());
  EXPECT_FALSE(srs.IsLocal());
  EXPECT_FALSE(srs.IsVertical());
  EXPECT_FALSE(srs.IsCompound());
}

TEST(OgrSpatialReferenceTest, CStringConstructor) {
  // Test construction with a short char *.
  OGRSpatialReference srs(R"WKT(
      GEOGCS["GCS_WGS_1966",
        DATUM["WGS_1966",
            SPHEROID["WGS_1966",6378145,298.25]],
        PRIMEM["Greenwich",0],
        UNIT["Degree",0.017453292519943295],
        AUTHORITY["EPSG","37001"]])WKT");
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_NE(nullptr, srs.GetRoot());
}

TEST(OgrSpatialReferenceTest, Wgs84) {
  // Tests the most common coordinate system.
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));  // WGS84
  EXPECT_NE(nullptr, srs.GetRoot());
  EXPECT_EQ(1, srs.GetReferenceCount());

  EXPECT_TRUE(srs.IsGeographic());
  EXPECT_FALSE(srs.IsProjected());
  EXPECT_FALSE(srs.IsGeocentric());
  EXPECT_FALSE(srs.IsLocal());
  EXPECT_FALSE(srs.IsVertical());
  EXPECT_FALSE(srs.IsCompound());
}

TEST(OgrSpatialReferenceTest, ReferenceCounting) {
  // Tests basic operation of the custom ref counting for this class.
  // TODO(schwehr): Figure out what the symmantics for reference counting are.
  OGRSpatialReference srs;
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_EQ(2, srs.Reference());
  EXPECT_EQ(2, srs.GetReferenceCount());
  EXPECT_EQ(1, srs.Dereference());
  EXPECT_EQ(1, srs.GetReferenceCount());
  // Calling srs.Release() here will crash.
  EXPECT_EQ(0, srs.Dereference());
  EXPECT_EQ(0, srs.GetReferenceCount());
  EXPECT_EQ(-1, srs.Dereference());
  EXPECT_EQ(-1, srs.GetReferenceCount());
  // Calling srs.Release() here will crash.
}

TEST(OgrSpatialReferenceTest, IsSameSphereoidPrecisionCheck) {
  // GDAL 2 rounds the last parameter of the SPHEROID.  Why?
  const string wkt_start = R"WKT(
      PROJCS["OSGB 1936 / British National Grid",
          GEOGCS["OSGB 1936",
              DATUM["OSGB_1936",
                  SPHEROID["Airy 1830",6377563.396,299.3249646)WKT";
  const string wkt_middle = "000043";
  const string wkt_end = R"WKT(
      ,
                      AUTHORITY["EPSG","7001"]],
                  TOWGS84[446.448,-125.157,542.06,0.15,0.247,0.842,-20.489],
                  AUTHORITY["EPSG","6277"]],
              PRIMEM["Greenwich",0,
                  AUTHORITY["EPSG","8901"]],
              UNIT["degree",0.0174532925199433,
                  AUTHORITY["EPSG","9122"]],
              AUTHORITY["EPSG","4277"]],
          PROJECTION["Transverse_Mercator"],
          PARAMETER["latitude_of_origin",49],
          PARAMETER["central_meridian",-2],
          PARAMETER["scale_factor",0.9996012717],
          PARAMETER["false_easting",400000],
          PARAMETER["false_northing",-100000],
          UNIT["metre",1,
              AUTHORITY["EPSG","9001"]],
          AXIS["Easting",EAST],
          AXIS["Northing",NORTH],
          AUTHORITY["EPSG","27700"]])WKT";

  OGRSpatialReference srs_a((wkt_start + wkt_end).c_str());
  OGRSpatialReference srs_b((wkt_start + wkt_middle + wkt_end).c_str());
  EXPECT_TRUE(srs_a.IsSame(&srs_b));
}

TEST(OgrSpatialReferenceTest, IsSameCloser) {
  const string wkt_start = R"WKT(
      PROJCS["OSGB 1936 / British National Grid",
          GEOGCS["OSGB 1936",
              DATUM["OSGB_1936",
                  SPHEROID["Airy 1830",6377563.396,299.3249646000043,
                      AUTHORITY["EPSG","7001"]],)WKT";
  // TOWGS84 is new with GDAL 2.
  const string wkt_towgs84 =
      "           TOWGS84[446.448,-125.157,542.06,0.15,0.247,0.842,-20.489],";
  const string wkt_end = R"WKT(
                  AUTHORITY["EPSG","6277"]],
              PRIMEM["Greenwich",0],
              UNIT["degree",0.0174532925199433],
              AUTHORITY["EPSG","4277"]],
          PROJECTION["Transverse_Mercator"],
          PARAMETER["latitude_of_origin",49],
          PARAMETER["central_meridian",-2],
          PARAMETER["scale_factor",0.9996012717],
          PARAMETER["false_easting",400000],
          PARAMETER["false_northing",-100000],
          UNIT["metre",1,
              AUTHORITY["EPSG","9001"]],
               AUTHORITY["EPSG","27700"]])WKT";

  const string wkt_gdal2 = R"WKT(
      PROJCS["OSGB 1936 / British National Grid",
          GEOGCS["OSGB 1936",
              DATUM["OSGB_1936",
                  SPHEROID["Airy 1830",6377563.396,299.3249646,
                      AUTHORITY["EPSG","7001"]],
                  TOWGS84[446.448,-125.157,542.06,0.15,0.247,0.842,-20.489],
                  AUTHORITY["EPSG","6277"]],
              PRIMEM["Greenwich",0,
                  AUTHORITY["EPSG","8901"]],
              UNIT["degree",0.0174532925199433,
                  AUTHORITY["EPSG","9122"]],
              AUTHORITY["EPSG","4277"]],
          PROJECTION["Transverse_Mercator"],
          PARAMETER["latitude_of_origin",49],
          PARAMETER["central_meridian",-2],
          PARAMETER["scale_factor",0.9996012717],
          PARAMETER["false_easting",400000],
          PARAMETER["false_northing",-100000],
          UNIT["metre",1,
              AUTHORITY["EPSG","9001"]],
          AXIS["Easting",EAST],
          AXIS["Northing",NORTH],
          AUTHORITY["EPSG","27700"]])WKT";

  OGRSpatialReference srs_old((wkt_start + wkt_end).c_str());
  OGRSpatialReference srs_old_with_towgs84(
      (wkt_start + wkt_towgs84 + wkt_end).c_str());
  OGRSpatialReference srs_new(wkt_gdal2.c_str());

  EXPECT_FALSE(srs_new.IsSame(&srs_old));
  EXPECT_TRUE(srs_new.IsSame(&srs_old_with_towgs84));
}

TEST(OgrSpatialReferenceTest, DifferByProj4Extension) {
  // Test IsSame() on SRS that differs only by their PROJ4 EXTENSION (besides
  // different EPSG codes).
  // Upstream: osr_epsg_12
  // https://trac.osgeo.org/gdal/ticket/7029

  // web_mercator
  OGRSpatialReference srs3857;
  EXPECT_EQ(OGRERR_NONE, srs3857.importFromEPSG(3857));

  // WGS84 Mercator
  OGRSpatialReference srs3395;
  EXPECT_EQ(OGRERR_NONE, srs3395.importFromEPSG(3395));

  EXPECT_FALSE(srs3395.IsSame(&srs3857));

  EXPECT_TRUE(srs3857.IsSame(&srs3857));
  EXPECT_TRUE(srs3395.IsSame(&srs3395));
}

TEST(OgrSpatialReferenceTest, InvalidCentralMeridian) {
  // Autofuzz POC from b/65413187
  const char kWkt[] = R"wkt(
      PROJCS["unnamed",GEOGCS["WGS 84",DATUM["WGS_1984",
      SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],
      AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],
      UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],
      AUTHORITY["EPSG","4326"]],PROJECTION["Transverse_Mercator"],
      PARAMETER["latitude_of_origin",0],
      PARAMETER["central_meridian",77777777777],
      PARAMETER["scale_factor",0.9996],
      PARAMETER["false_easting","nan"],
      PARAMETER["false_northing",0],UNIT["Go]",1]])wkt";
  OGRSpatialReference srs(kWkt);
  int north = TRUE;
  EXPECT_EQ(0, srs.GetUTMZone(&north));

  // This formerly triggered an ASAN violation.
  srs.morphToESRI();
}

// TODO(schwehr): Write many more tests.

}  // namespace

