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
// https://github.com/OSGeo/gdal/blob/master/autotest/osr/osr_xml.py

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"

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
  EXPECT_THAT(wgs84, IsSameAs(srs));

  char* wkt = nullptr;
  srs.exportToWkt(&wkt);
  EXPECT_THAT(std::string(wkt), testing::HasSubstr("4326"));
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

  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.Validate());

  OGRSpatialReference srs32748;
  srs32748.importFromEPSG(32748);
  EXPECT_THAT(srs32748, IsSameAs(srs));

  char* wkt = nullptr;
  srs.exportToWkt(&wkt);
  EXPECT_THAT(std::string(wkt), testing::HasSubstr("32748"));
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
  EXPECT_THAT(srs4168, IsSameAs(srs));

  char* wkt = nullptr;
  srs.exportToWkt(&wkt);
  EXPECT_THAT(std::string(wkt), testing::HasSubstr("4168"));
  CPLFree(wkt);
  OSRCleanup();
}

TEST(SrsXmlTest, ExportToXmlGeographic) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  char* xml = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToXML(&xml, nullptr));
  ASSERT_NE(xml, nullptr);
  EXPECT_THAT(xml, testing::HasSubstr("GeographicCRS"));
  EXPECT_THAT(xml, testing::HasSubstr("WGS 84"));

  // Roundtrip
  OGRSpatialReference srsRoundtrip;
  EXPECT_EQ(OGRERR_NONE, srsRoundtrip.importFromXML(xml));
  EXPECT_THAT(srs, IsSameAs(srsRoundtrip));
  CPLFree(xml);

  // C API export and import
  char* xmlC = nullptr;
  EXPECT_EQ(OGRERR_NONE, OSRExportToXML(OGRSpatialReference::ToHandle(&srs),
                                        &xmlC, nullptr));
  ASSERT_NE(xmlC, nullptr);
  OGRSpatialReference srsC;
  EXPECT_EQ(OGRERR_NONE,
            OSRImportFromXML(OGRSpatialReference::ToHandle(&srsC), xmlC));
  EXPECT_THAT(srs, IsSameAs(srsC));
  CPLFree(xmlC);
  OSRCleanup();
}

TEST(SrsXmlTest, ExportToXmlProjectedTM) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32611));

  char* xml = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToXML(&xml, nullptr));
  ASSERT_NE(xml, nullptr);
  EXPECT_THAT(xml, testing::HasSubstr("ProjectedCRS"));
  EXPECT_THAT(xml, testing::HasSubstr("Conversion"));

  OGRSpatialReference srsRoundtrip;
  EXPECT_EQ(OGRERR_NONE, srsRoundtrip.importFromXML(xml));
  EXPECT_THAT(srs, IsSameAs(srsRoundtrip));
  CPLFree(xml);
  OSRCleanup();
}

TEST(SrsXmlTest, ExportToXmlProjectedLCC) {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  srs.SetLCC1SP(10.0, 20.0, 0.99, 500000.0, 1000000.0);

  char* xml = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToXML(&xml, nullptr));
  ASSERT_NE(xml, nullptr);
  EXPECT_THAT(xml, testing::HasSubstr("ProjectedCRS"));
  EXPECT_THAT(xml, testing::HasSubstr("9801"));
  CPLFree(xml);
  OSRCleanup();
}

TEST(SrsXmlTest, ExportToXmlErrorCases) {
  char* xml = nullptr;
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToXML(nullptr, &xml, nullptr));

  // Local CS is unsupported for GML export
  OGRSpatialReference srsLocal;
  srsLocal.SetLocalCS("MyLocalCS");
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srsLocal.exportToXML(&xml, nullptr));

  // Unsupported projection method (e.g. Mercator)
  OGRSpatialReference srsMerc;
  srsMerc.SetWellKnownGeogCS("WGS84");
  srsMerc.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
  {
    WithQuietHandler q;
    EXPECT_EQ(OGRERR_FAILURE, srsMerc.exportToXML(&xml, nullptr));
  }

  // Projected CS missing PROJECTION node
  OGRSpatialReference srsTM;
  ASSERT_EQ(OGRERR_NONE, srsTM.importFromEPSG(32611));
  srsTM.GetAttrNode("PROJCS")->StripNodes("PROJECTION");
  {
    WithQuietHandler q;
    EXPECT_NE(OGRERR_NONE, srsTM.exportToXML(&xml, nullptr));
  }

  // Geographic CS missing DATUM node
  OGRSpatialReference srsGCS;
  ASSERT_EQ(OGRERR_NONE, srsGCS.importFromEPSG(4326));
  srsGCS.GetAttrNode("GEOGCS")->StripNodes("DATUM");
  EXPECT_NE(OGRERR_NONE, srsGCS.exportToXML(&xml, nullptr));

  OSRCleanup();
}

TEST(SrsXmlTest, ImportFromXmlErrorCasesAndVariants) {
  WithQuietHandler q;
  EXPECT_EQ(OGRERR_FAILURE, OSRImportFromXML(nullptr, "<xml/>"));

  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_FAILURE,
            OSRImportFromXML(OGRSpatialReference::ToHandle(&srs), nullptr));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromXML("<open><unclosed>"));
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.importFromXML("<gml:UnsupportedCRS/>"));

  // GeographicCRS without datum but with EPSG code in srsID
  constexpr char kEpsgOnlyGcs[] = R"gml(
    <gml:GeographicCRS>
      <gml:srsName>WGS 84</gml:srsName>
      <gml:srsID>
        <gml:name codeSpace="urn:ogc:def:crs:EPSG::">4326</gml:name>
      </gml:srsID>
    </gml:GeographicCRS>
  )gml";
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kEpsgOnlyGcs));
  EXPECT_TRUE(srs.IsGeographic());
  EXPECT_STREQ(srs.GetAuthorityCode("GEOGCS"), "4326");

  // GeographicCRS with missing inverseFlattening -> corrupt data
  constexpr char kMissingInvFlattening[] = R"gml(
    <gml:GeographicCRS>
      <gml:srsName>BadEllips</gml:srsName>
      <gml:usesGeodeticDatum>
        <gml:GeodeticDatum>
          <gml:datumName>D</gml:datumName>
          <gml:usesEllipsoid>
            <gml:Ellipsoid>
              <gml:ellipsoidName>E</gml:ellipsoidName>
              <gml:semiMajorAxis>6378137</gml:semiMajorAxis>
            </gml:Ellipsoid>
          </gml:usesEllipsoid>
        </gml:GeodeticDatum>
      </gml:usesGeodeticDatum>
    </gml:GeographicCRS>
  )gml";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromXML(kMissingInvFlattening));

  // GeographicCRS without prime meridian defaults to Greenwich 0.0
  constexpr char kNoPrimeMeridian[] = R"gml(
    <gml:GeographicCRS>
      <gml:srsName>NoPM</gml:srsName>
      <gml:usesGeodeticDatum>
        <gml:GeodeticDatum>
          <gml:datumName>D</gml:datumName>
          <gml:usesEllipsoid>
            <gml:Ellipsoid>
              <gml:ellipsoidName>E</gml:ellipsoidName>
              <gml:semiMajorAxis>6378137</gml:semiMajorAxis>
              <gml:secondDefiningParameter>
                <gml:inverseFlattening>298.257223563</gml:inverseFlattening>
              </gml:secondDefiningParameter>
            </gml:Ellipsoid>
          </gml:usesEllipsoid>
        </gml:GeodeticDatum>
      </gml:usesGeodeticDatum>
    </gml:GeographicCRS>
  )gml";
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kNoPrimeMeridian));
  EXPECT_DOUBLE_EQ(srs.GetPrimeMeridian(), 0.0);

  // ProjectedCRS without definedByConversion but with EPSG code in srsID
  constexpr char kEpsgOnlyPcs[] = R"gml(
    <gml:ProjectedCRS>
      <gml:srsName>UTM 11N</gml:srsName>
      <gml:srsID>
        <gml:name codeSpace="urn:ogc:def:crs:EPSG::">32611</gml:name>
      </gml:srsID>
    </gml:ProjectedCRS>
  )gml";
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kEpsgOnlyPcs));
  EXPECT_TRUE(srs.IsProjected());
  EXPECT_STREQ(srs.GetAuthorityCode("PROJCS"), "32611");

  // ProjectedCRS without definedByConversion and no EPSG code -> corrupt
  constexpr char kNoConvNoEpsg[] = R"gml(
    <gml:ProjectedCRS>
      <gml:srsName>NoConv</gml:srsName>
    </gml:ProjectedCRS>
  )gml";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromXML(kNoConvNoEpsg));

  // ProjectedCRS with unrecognized conversion method -> corrupt
  constexpr char kBadMethod[] = R"gml(
    <gml:ProjectedCRS>
      <gml:srsName>BadMethod</gml:srsName>
      <gml:definedByConversion>
        <gml:Conversion>
          <gml:usesMethod xlink:href="urn:ogc:def:method:EPSG::9999"/>
        </gml:Conversion>
      </gml:definedByConversion>
    </gml:ProjectedCRS>
  )gml";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromXML(kBadMethod));

  // ProjectedCRS with href attribute (no xlink:), method code as text node,
  // and parameter without value child
  constexpr char kHrefMethodTextNode[] = R"gml(
    <gml:ProjectedCRS>
      <gml:srsName>CustomTM</gml:srsName>
      <gml:definedByConversion>
        <gml:Conversion>
          <gml:usesMethod href="urn:ogc:def:method:EPSG::">9807</gml:usesMethod>
          <gml:usesValue>
            <gml:valueOfParameter xlink:href="urn:ogc:def:parameter:EPSG::8801"/>
          </gml:usesValue>
        </gml:Conversion>
      </gml:definedByConversion>
    </gml:ProjectedCRS>
  )gml";
  EXPECT_EQ(OGRERR_NONE, srs.importFromXML(kHrefMethodTextNode));

  // ProjectedCRS with corrupt baseCRS.GeographicCRS
  constexpr char kBadBaseCRS[] = R"gml(
    <gml:ProjectedCRS>
      <gml:srsName>BadBase</gml:srsName>
      <gml:baseCRS>
        <gml:GeographicCRS>
          <gml:usesGeodeticDatum>
            <gml:GeodeticDatum>
              <gml:usesEllipsoid>
                <gml:Ellipsoid>
                  <gml:semiMajorAxis>6378137</gml:semiMajorAxis>
                </gml:Ellipsoid>
              </gml:usesEllipsoid>
            </gml:GeodeticDatum>
          </gml:usesGeodeticDatum>
        </gml:GeographicCRS>
      </gml:baseCRS>
      <gml:definedByConversion>
        <gml:Conversion>
          <gml:usesMethod xlink:href="urn:ogc:def:method:EPSG::9807"/>
        </gml:Conversion>
      </gml:definedByConversion>
    </gml:ProjectedCRS>
  )gml";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromXML(kBadBaseCRS));

  // Malformed URN variations in srsID
  for (const char* urn : {"not_a_urn", "urn:ogc:def:crs",
                          "urn:ogc:def:crs:EPSG", "urn:ogc:def:crs:EPSG:"}) {
    std::string malformed = CPLSPrintf(
        R"gml(<gml:GeographicCRS><gml:srsName>M</gml:srsName><gml:srsID><gml:name codeSpace="%s">4326</gml:name></gml:srsID></gml:GeographicCRS>)gml",
        urn);
    EXPECT_NE(OGRERR_NONE, srs.importFromXML(malformed.c_str()));
  }

  OSRCleanup();
}

}  // namespace
}  // namespace autotest2
