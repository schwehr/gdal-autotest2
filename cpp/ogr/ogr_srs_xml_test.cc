// Copyright 2017 Google Inc. All Rights Reserved.
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
// Copyright (c) 2009, Even Rouault <even dot rouault at mines-paris dot org>
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// https://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_xml.py

#include "ogr/ogr_spatialref.h"

#include <string>

#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "port/cpl_conv.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {


TEST(SrsXmlTest, EmptyString) {
  OGRSpatialReference srs;
  // Totally bogus string.
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromXML(""));
}

TEST(SrsXmlTest, Wgs84) {
  // Geographic
  constexpr char kWgs84Xml[] = R"gml(
  <gml:GeographicCRS gml:id="ogrcrs8">
  <gml:srsName>WGS 84</gml:srsName>
  <gml:srsID>
    <gml:name codeSpace="urn:ogc:def:crs:EPSG::">4326</gml:name>
  </gml:srsID>
  <gml:usesEllipsoidalCS>
    <gml:EllipsoidalCS gml:id="ogrcrs9">
      <gml:csName>ellipsoidal</gml:csName>
      <gml:csID>
        <gml:name codeSpace="urn:ogc:def:cs:EPSG::">6402</gml:name>
      </gml:csID>
      <gml:usesAxis>
        <gml:CoordinateSystemAxis gml:id="ogrcrs10" gml:uom="urn:ogc:def:uom:EPSG::9102">
          <gml:name>Geodetic latitude</gml:name>
          <gml:axisID>
            <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9901</gml:name>
          </gml:axisID>
          <gml:axisAbbrev>Lat</gml:axisAbbrev>
          <gml:axisDirection>north</gml:axisDirection>
        </gml:CoordinateSystemAxis>
      </gml:usesAxis>
      <gml:usesAxis>
        <gml:CoordinateSystemAxis gml:id="ogrcrs11" gml:uom="urn:ogc:def:uom:EPSG::9102">
          <gml:name>Geodetic longitude</gml:name>
          <gml:axisID>
            <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9902</gml:name>
          </gml:axisID>
          <gml:axisAbbrev>Lon</gml:axisAbbrev>
          <gml:axisDirection>east</gml:axisDirection>
        </gml:CoordinateSystemAxis>
      </gml:usesAxis>
    </gml:EllipsoidalCS>
  </gml:usesEllipsoidalCS>
  <gml:usesGeodeticDatum>
    <gml:GeodeticDatum gml:id="ogrcrs12">
      <gml:datumName>WGS_1984</gml:datumName>
      <gml:datumID>
        <gml:name codeSpace="urn:ogc:def:datum:EPSG::">6326</gml:name>
      </gml:datumID>
      <gml:usesPrimeMeridian>
        <gml:PrimeMeridian gml:id="ogrcrs13">
          <gml:meridianName>Greenwich</gml:meridianName>
          <gml:meridianID>
            <gml:name codeSpace="urn:ogc:def:meridian:EPSG::">8901</gml:name>
          </gml:meridianID>
          <gml:greenwichLongitude>
            <gml:angle uom="urn:ogc:def:uom:EPSG::9102">0</gml:angle>
          </gml:greenwichLongitude>
        </gml:PrimeMeridian>
      </gml:usesPrimeMeridian>
      <gml:usesEllipsoid>
        <gml:Ellipsoid gml:id="ogrcrs14">
          <gml:ellipsoidName>WGS 84</gml:ellipsoidName>
          <gml:ellipsoidID>
            <gml:name codeSpace="urn:ogc:def:ellipsoid:EPSG::">7030</gml:name>
          </gml:ellipsoidID>
          <gml:semiMajorAxis uom="urn:ogc:def:uom:EPSG::9001">6378137</gml:semiMajorAxis>
          <gml:secondDefiningParameter>
            <gml:inverseFlattening uom="urn:ogc:def:uom:EPSG::9201">298.257223563</gml:inverseFlattening>
          </gml:secondDefiningParameter>
        </gml:Ellipsoid>
      </gml:usesEllipsoid>
    </gml:GeodeticDatum>
  </gml:usesGeodeticDatum>
  </gml:GeographicCRS>
  )gml";
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kWgs84Xml));

  OGRSpatialReference wgs84;
  wgs84.importFromEPSG(4326);
  EXPECT_TRUE(wgs84.IsSame(&srs));

  char *wkt = nullptr;
  srs.exportToWkt(&wkt);
  EXPECT_THAT(string(wkt), testing::HasSubstr("4326"));
  CPLFree(wkt);
}

TEST(SrsXmlTest, Epsg32748UtmZone48S_Wgs84) {
  constexpr char kEpsg32748[] = R"gml(
  <gml:ProjectedCRS gml:id="ogrcrs15">
  <gml:srsName>WGS 84 / UTM zone 48S</gml:srsName>
  <gml:srsID>
    <gml:name codeSpace="urn:ogc:def:crs:EPSG::">32748</gml:name>
  </gml:srsID>
  <gml:baseCRS>
    <gml:GeographicCRS gml:id="ogrcrs16">
      <gml:srsName>WGS 84</gml:srsName>
      <gml:srsID>
        <gml:name codeSpace="urn:ogc:def:crs:EPSG::">4326</gml:name>
      </gml:srsID>
      <gml:usesEllipsoidalCS>
        <gml:EllipsoidalCS gml:id="ogrcrs17">
          <gml:csName>ellipsoidal</gml:csName>
          <gml:csID>
            <gml:name codeSpace="urn:ogc:def:cs:EPSG::">6402</gml:name>
          </gml:csID>
          <gml:usesAxis>
            <gml:CoordinateSystemAxis gml:id="ogrcrs18" gml:uom="urn:ogc:def:uom:EPSG::9102">
              <gml:name>Geodetic latitude</gml:name>
              <gml:axisID>
                <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9901</gml:name>
              </gml:axisID>
              <gml:axisAbbrev>Lat</gml:axisAbbrev>
              <gml:axisDirection>north</gml:axisDirection>
            </gml:CoordinateSystemAxis>
          </gml:usesAxis>
          <gml:usesAxis>
            <gml:CoordinateSystemAxis gml:id="ogrcrs19" gml:uom="urn:ogc:def:uom:EPSG::9102">
              <gml:name>Geodetic longitude</gml:name>
              <gml:axisID>
                <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9902</gml:name>
              </gml:axisID>
              <gml:axisAbbrev>Lon</gml:axisAbbrev>
              <gml:axisDirection>east</gml:axisDirection>
            </gml:CoordinateSystemAxis>
          </gml:usesAxis>
        </gml:EllipsoidalCS>
      </gml:usesEllipsoidalCS>
      <gml:usesGeodeticDatum>
        <gml:GeodeticDatum gml:id="ogrcrs20">
          <gml:datumName>WGS_1984</gml:datumName>
          <gml:datumID>
            <gml:name codeSpace="urn:ogc:def:datum:EPSG::">6326</gml:name>
          </gml:datumID>
          <gml:usesPrimeMeridian>
            <gml:PrimeMeridian gml:id="ogrcrs21">
              <gml:meridianName>Greenwich</gml:meridianName>
              <gml:meridianID>
                <gml:name codeSpace="urn:ogc:def:meridian:EPSG::">8901</gml:name>
              </gml:meridianID>
              <gml:greenwichLongitude>
                <gml:angle uom="urn:ogc:def:uom:EPSG::9102">0</gml:angle>
              </gml:greenwichLongitude>
            </gml:PrimeMeridian>
          </gml:usesPrimeMeridian>
          <gml:usesEllipsoid>
            <gml:Ellipsoid gml:id="ogrcrs22">
              <gml:ellipsoidName>WGS 84</gml:ellipsoidName>
              <gml:ellipsoidID>
                <gml:name codeSpace="urn:ogc:def:ellipsoid:EPSG::">7030</gml:name>
              </gml:ellipsoidID>
              <gml:semiMajorAxis uom="urn:ogc:def:uom:EPSG::9001">6378137</gml:semiMajorAxis>
              <gml:secondDefiningParameter>
                <gml:inverseFlattening uom="urn:ogc:def:uom:EPSG::9201">298.257223563</gml:inverseFlattening>
              </gml:secondDefiningParameter>
            </gml:Ellipsoid>
          </gml:usesEllipsoid>
        </gml:GeodeticDatum>
      </gml:usesGeodeticDatum>
    </gml:GeographicCRS>
  </gml:baseCRS>
  <gml:definedByConversion>
    <gml:Conversion gml:id="ogrcrs23">
      <gml:coordinateOperationName>Transverse_Mercator</gml:coordinateOperationName>
      <gml:usesMethod xlink:href="urn:ogc:def:method:EPSG::9807" />
      <gml:usesValue>
        <gml:value uom="urn:ogc:def:uom:EPSG::9102">0</gml:value>
        <gml:valueOfParameter xlink:href="urn:ogc:def:parameter:EPSG::8801" />
      </gml:usesValue>
      <gml:usesValue>
        <gml:value uom="urn:ogc:def:uom:EPSG::9102">105</gml:value>
        <gml:valueOfParameter xlink:href="urn:ogc:def:parameter:EPSG::8802" />
      </gml:usesValue>
      <gml:usesValue>
        <gml:value uom="urn:ogc:def:uom:EPSG::9001">0.9996</gml:value>
        <gml:valueOfParameter xlink:href="urn:ogc:def:parameter:EPSG::8805" />
      </gml:usesValue>
      <gml:usesValue>
        <gml:value uom="urn:ogc:def:uom:EPSG::9001">500000</gml:value>
        <gml:valueOfParameter xlink:href="urn:ogc:def:parameter:EPSG::8806" />
      </gml:usesValue>
      <gml:usesValue>
        <gml:value uom="urn:ogc:def:uom:EPSG::9001">10000000</gml:value>
        <gml:valueOfParameter xlink:href="urn:ogc:def:parameter:EPSG::8807" />
      </gml:usesValue>
    </gml:Conversion>
  </gml:definedByConversion>
  <gml:usesCartesianCS>
    <gml:CartesianCS gml:id="ogrcrs24">
      <gml:csName>Cartesian</gml:csName>
      <gml:csID>
        <gml:name codeSpace="urn:ogc:def:cs:EPSG::">4400</gml:name>
      </gml:csID>
      <gml:usesAxis>
        <gml:CoordinateSystemAxis gml:id="ogrcrs25" gml:uom="urn:ogc:def:uom:EPSG::9001">
          <gml:name>Easting</gml:name>
          <gml:axisID>
            <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9906</gml:name>
          </gml:axisID>
          <gml:axisAbbrev>E</gml:axisAbbrev>
          <gml:axisDirection>east</gml:axisDirection>
        </gml:CoordinateSystemAxis>
      </gml:usesAxis>
      <gml:usesAxis>
        <gml:CoordinateSystemAxis gml:id="ogrcrs26" gml:uom="urn:ogc:def:uom:EPSG::9001">
          <gml:name>Northing</gml:name>
          <gml:axisID>
            <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9907</gml:name>
          </gml:axisID>
          <gml:axisAbbrev>N</gml:axisAbbrev>
          <gml:axisDirection>north</gml:axisDirection>
        </gml:CoordinateSystemAxis>
      </gml:usesAxis>
    </gml:CartesianCS>
  </gml:usesCartesianCS>
</gml:ProjectedCRS>)gml";
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kEpsg32748));

  // TODO(schwehr): Why does it not validate?
  EXPECT_FALSE(srs.Validate());

  OGRSpatialReference srs32748;
  srs32748.importFromEPSG(32748);
  EXPECT_TRUE(srs32748.IsSame(&srs));

  char *wkt = nullptr;
  srs.exportToWkt(&wkt);
  EXPECT_THAT(string(wkt), testing::HasSubstr("32748"));
  CPLFree(wkt);
}

TEST(SrsXmlTest, Epsg4168) {
  constexpr char kEpsg4168[] = R"gml(
  <gml:GeographicCRS gml:id="ogrcrs11633">
  <gml:srsName>Accra</gml:srsName>
  <gml:srsID>
    <gml:name codeSpace="urn:ogc:def:crs:EPSG::">4168</gml:name>
  </gml:srsID>
  <gml:usesEllipsoidalCS>
    <gml:EllipsoidalCS gml:id="ogrcrs11634">
      <gml:csName>ellipsoidal</gml:csName>
      <gml:csID>
        <gml:name codeSpace="urn:ogc:def:cs:EPSG::">6402</gml:name>
      </gml:csID>
      <gml:usesAxis>
        <gml:CoordinateSystemAxis gml:id="ogrcrs11635" gml:uom="urn:ogc:def:uom:EPSG::9102">
          <gml:name>Geodetic latitude</gml:name>
          <gml:axisID>
            <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9901</gml:name>
          </gml:axisID>
          <gml:axisAbbrev>Lat</gml:axisAbbrev>
          <gml:axisDirection>north</gml:axisDirection>
        </gml:CoordinateSystemAxis>
      </gml:usesAxis>
      <gml:usesAxis>
        <gml:CoordinateSystemAxis gml:id="ogrcrs11636" gml:uom="urn:ogc:def:uom:EPSG::9102">
          <gml:name>Geodetic longitude</gml:name>
          <gml:axisID>
            <gml:name codeSpace="urn:ogc:def:axis:EPSG::">9902</gml:name>
          </gml:axisID>
          <gml:axisAbbrev>Lon</gml:axisAbbrev>
          <gml:axisDirection>east</gml:axisDirection>
        </gml:CoordinateSystemAxis>
      </gml:usesAxis>
    </gml:EllipsoidalCS>
  </gml:usesEllipsoidalCS>
  <gml:usesGeodeticDatum>
    <gml:GeodeticDatum gml:id="ogrcrs11637">
      <gml:datumName>Accra</gml:datumName>
      <gml:datumID>
        <gml:name codeSpace="urn:ogc:def:datum:EPSG::">6168</gml:name>
      </gml:datumID>
      <gml:usesPrimeMeridian>
        <gml:PrimeMeridian gml:id="ogrcrs11638">
          <gml:meridianName>Greenwich</gml:meridianName>
          <gml:meridianID>
            <gml:name codeSpace="urn:ogc:def:meridian:EPSG::">8901</gml:name>
          </gml:meridianID>
          <gml:greenwichLongitude>
            <gml:angle uom="urn:ogc:def:uom:EPSG::9102">0</gml:angle>
          </gml:greenwichLongitude>
        </gml:PrimeMeridian>
      </gml:usesPrimeMeridian>
      <gml:usesEllipsoid>
        <gml:Ellipsoid gml:id="ogrcrs11639">
          <gml:ellipsoidName>War Office</gml:ellipsoidName>
          <gml:ellipsoidID>
            <gml:name codeSpace="urn:ogc:def:ellipsoid:EPSG::">7029</gml:name>
          </gml:ellipsoidID>
          <gml:semiMajorAxis uom="urn:ogc:def:uom:EPSG::9001">6378300</gml:semiMajorAxis>
          <gml:secondDefiningParameter>
            <gml:inverseFlattening uom="urn:ogc:def:uom:EPSG::9201">296</gml:inverseFlattening>
          </gml:secondDefiningParameter>
        </gml:Ellipsoid>
      </gml:usesEllipsoid>
    </gml:GeodeticDatum>
  </gml:usesGeodeticDatum>
  </gml:GeographicCRS>
  )gml";
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kEpsg4168));

  // TODO(schwehr): Why does it not validate?
  EXPECT_FALSE(srs.Validate());

  OGRSpatialReference srs4168;
  srs4168.importFromEPSG(4168);
  // TODO(schwehr): Why?
  EXPECT_FALSE(srs4168.IsSame(&srs));

  char *wkt = nullptr;
  srs.exportToWkt(&wkt);
  EXPECT_THAT(string(wkt), testing::HasSubstr("4168"));
  CPLFree(wkt);
}

}  // namespace
}  // namespace autotest2
