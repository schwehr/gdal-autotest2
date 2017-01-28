#!/usr/bin/env python
# Copyright 2014 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

"""Test basic spatial reference system.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_basic.py
"""

import unittest


from osgeo import osr
import unittest


class OsrBasic(unittest.TestCase):

  def testBasic01Wgs84(self):
    srs = osr.SpatialReference()
    srs.SetUTM(11, 0)  # Southern Hemisphere.
    srs.SetWellKnownGeogCS('WGS84')
    self.assertEqual(srs.GetUTMZone(), -11)
    srs.SetUTM(11)  # Northern Hemisphere.
    self.assertEqual(srs.GetUTMZone(), 11)

    parms = (
        (osr.SRS_PP_CENTRAL_MERIDIAN, -117.0),
        (osr.SRS_PP_LATITUDE_OF_ORIGIN, 0.0),
        (osr.SRS_PP_SCALE_FACTOR, 0.9996),
        (osr.SRS_PP_FALSE_EASTING, 500000.0),
        (osr.SRS_PP_FALSE_NORTHING, 0.0)
        )

    for parm, expected in parms:
      value = srs.GetProjParm(parm, -999)
      self.assertAlmostEqual(value, expected)

    for auth, expected in (('GEOGCS', '4326'), ('DATUM', '6326')):
      self.assertEqual(srs.GetAuthorityName(auth), 'EPSG')
      code = srs.GetAuthorityCode(auth)
      self.assertEqual(code, expected)

  def testBasic02Nad83StatePlane(self):
    srs = osr.SpatialReference()
    srs.SetStatePlane(403, 1)  # California III NAD83.
    parms = (
        (osr.SRS_PP_STANDARD_PARALLEL_1, 38.43333333333333),
        (osr.SRS_PP_STANDARD_PARALLEL_2, 37.06666666666667),
        (osr.SRS_PP_LATITUDE_OF_ORIGIN, 36.5),
        (osr.SRS_PP_CENTRAL_MERIDIAN, -120.5),
        (osr.SRS_PP_FALSE_EASTING, 2000000.0),
        (osr.SRS_PP_FALSE_NORTHING, 500000.0)
        )

    for parm, expected in parms:
      value = srs.GetProjParm(parm, -999)
      self.assertAlmostEqual(value, expected)

    auths = (('GEOGCS', '4269'),
             ('DATUM', '6269'),
             ('PROJCS', '26943'),
             ('PROJCS|UNIT', '9001'))
    for auth, expected in auths:
      self.assertEqual(srs.GetAuthorityName(auth), 'EPSG')
      code = srs.GetAuthorityCode(auth)
      self.assertEqual(code, expected)

  def testBasic03StatePlaneWithFeet(self):
    srs = osr.SpatialReference()
    srs.SetStatePlane(403, 1, 'Foot', 0.3048006096012192)
    parms = (
        (osr.SRS_PP_STANDARD_PARALLEL_1, 38.43333333333333),
        (osr.SRS_PP_STANDARD_PARALLEL_2, 37.06666666666667),
        (osr.SRS_PP_LATITUDE_OF_ORIGIN, 36.5),
        (osr.SRS_PP_CENTRAL_MERIDIAN, -120.5),
        (osr.SRS_PP_FALSE_EASTING, 6561666.666666667),
        (osr.SRS_PP_FALSE_NORTHING, 1640416.666666667)
        )
    for parm, expected in parms:
      value = srs.GetProjParm(parm, -999)
      self.assertAlmostEqual(value, expected)

    auths = (('GEOGCS', '4269'), ('DATUM', '6269'))
    for auth, expected in auths:
      self.assertEqual(srs.GetAuthorityName(auth), 'EPSG')
      code = srs.GetAuthorityCode(auth)
      self.assertEqual(code, expected)

    self.assertIsNone(srs.GetAuthorityName('PROJCS'))
    # TODO(schwehr): PROJCS|UNIT returns none.  Should it return something?
    self.assertNotEqual(srs.GetAuthorityCode('PROJCS|UNIT'), 9001)
    self.assertIsNone(srs.GetAuthorityCode('PROJCS|UNIT'))
    self.assertEqual(srs.GetLinearUnitsName(), 'Foot')

  def testBasic04TranslateNadShiftToProj4(self):
    srs = osr.SpatialReference()
    srs.SetGS(cm=-117.0, fe=100000.0, fn=100000)
    srs.SetGeogCS('Test GCS', 'Test Datum', 'WGS84',
                  osr.SRS_WGS84_SEMIMAJOR, osr.SRS_WGS84_INVFLATTENING)
    srs.SetTOWGS84(1, 2, 3)
    self.assertEqual(srs.GetTOWGS84(), (1, 2, 3, 0, 0, 0, 0))

    proj4 = srs.ExportToProj4()
    srs2 = osr.SpatialReference()
    srs2.ImportFromProj4(proj4)

    self.assertEqual(srs.GetTOWGS84(), srs2.GetTOWGS84())

  def testBasic05UrlForOgcCrs84(self):
    self.assertEqual(osr.GetUserInputAsWKT('urn:ogc:def:crs:OGC:1.3:CRS84'),
                     osr.GetUserInputAsWKT('WGS84'))

  def testBasic06UrnSupportForEpsg(self):
    # Try without version, with version, and no version without repeate.
    # Last is probably illegal based on
    # http://www.opengeospatial.org/ogcUrnPolicy, but found quite often
    # in the wild especially in content returned by GeoServer.
    wkts = (
        osr.GetUserInputAsWKT('urn:x-ogc:def:crs:EPSG::4326'),
        osr.GetUserInputAsWKT('urn:x-ogc:def:crs:EPSG:6.6:4326'),
        osr.GetUserInputAsWKT('urn:x-ogc:def:crs:EPSG:4326')
        )
    for wkt in wkts:
      self.assertIn('GEOGCS["WGS 84",DATUM["WGS_1984"', wkt)
      self.assertIn('AXIS["Latitude",NORTH],AXIS["Longitude",EAST]', wkt)

  # TODO(schwehr): Brittle test design.
  def testBasic07UrnAutoProjection(self):
    self.assertEqual(
        osr.GetUserInputAsWKT('urn:ogc:def:crs:OGC::AUTO42001:-117:33'),
        'PROJCS["UTM Zone 11, Northern Hemisphere",GEOGCS["WGS 84",'
        'DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,'
        'AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AUTHORITY["EPSG","4326"]],PROJECTION["Transverse_Mercator"],'
        'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",-117],'
        'PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],'
        'PARAMETER["false_northing",0],UNIT["Meter",1,'
        'AUTHORITY["EPSG","9001"]]]')

  def testBasic08SetLinearUnitsAndUpdateParameters(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput('+proj=tmerc +x_0=1000 +datum=WGS84 +units=m')
    srs.SetLinearUnits('Foot', 0.3048)

    self.assertEqual(srs.GetProjParm('false_easting'), 1000)

    # TODO(schwehr): why would SetLinearUnitsAndUpdateParameters not be in srs?

    srs.SetFromUserInput('+proj=tmerc +x_0=1000 +datum=WGS84 +units=m')
    srs.SetLinearUnitsAndUpdateParameters('Foot', 0.3048)
    self.assertAlmostEqual(srs.GetProjParm('false_easting'), 3280.83989501)

  def testBasic09Validate(self):
    srs = osr.SpatialReference()
    wkt = (
        'PROJCS["unnamed",GEOGCS["unnamed ellipse",DATUM["unknown",SPHEROID'
        '["unnamed",6378137,0]],PRIMEM["Greenwich",0],UNIT["degree",'
        '0.0174532925199433]],PROJECTION["Mercator_2SP"],PARAMETER'
        '["standard_parallel_1",0],PARAMETER["latitude_of_origin",0],'
        'PARAMETER["central_meridian",0],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],UNIT["Meter",1],EXTENSION["PROJ4",'
        '"+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 '
        '+y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext  +no_defs"]]'
    )
    srs.SetFromUserInput(wkt)
    self.assertEqual(srs.Validate(), 0)

  def testBasic10ValidateOnProjCsWithAxis(self):
    # http://trac.osgeo.org/gdal/ticket/2739
    srs = osr.SpatialReference()
    wkt = (
        'PROJCS["NAD83(CSRS98) / UTM zone 20N (deprecated)",'
        '    GEOGCS["NAD83(CSRS98)",'
        '        DATUM["NAD83_Canadian_Spatial_Reference_System",'
        '            SPHEROID["GRS 1980",6378137,298.257222101,'
        '                AUTHORITY["EPSG","7019"]],'
        '            TOWGS84[0,0,0,0,0,0,0],'
        '            AUTHORITY["EPSG","6140"]],'
        '        PRIMEM["Greenwich",0,'
        '            AUTHORITY["EPSG","8901"]],'
        '        UNIT["degree",0.0174532925199433,'
        '            AUTHORITY["EPSG","9108"]],'
        '        AUTHORITY["EPSG","4140"]],'
        '    PROJECTION["Transverse_Mercator"],'
        '    PARAMETER["latitude_of_origin",0],'
        '    PARAMETER["central_meridian",-63],'
        '    PARAMETER["scale_factor",0.9996],'
        '    PARAMETER["false_easting",500000],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["metre",1,'
        '        AUTHORITY["EPSG","9001"]],'
        '    AXIS["Easting",EAST],'
        '    AXIS["Northing",NORTH],'
        '    AUTHORITY["EPSG","2038"]]'
    )
    srs.SetFromUserInput(wkt)
    self.assertEqual(srs.Validate(), 0)

  def testBasic11IsSameAndIsSameGeogCS(self):
    srs1 = osr.SpatialReference()
    srs2 = osr.SpatialReference()

    input1 = ('PROJCS["NAD83(CSRS98) / UTM zone 20N (deprecated)",'
              '    GEOGCS["NAD83(CSRS98)",'
              '        DATUM["NAD83_Canadian_Spatial_Reference_System",'
              '            SPHEROID["GRS 1980",6378137,298.257222101,'
              '                AUTHORITY["EPSG","7019"]],'
              '            TOWGS84[0,0,0,0,0,0,0],'
              '            AUTHORITY["EPSG","6140"]],'
              '        PRIMEM["Greenwich",0,'
              '            AUTHORITY["EPSG","8901"]],'
              '        UNIT["degree",0.0174532925199433,'
              '            AUTHORITY["EPSG","9108"]],'
              '        AUTHORITY["EPSG","4140"]],'
              '    PROJECTION["Transverse_Mercator"],'
              '    PARAMETER["latitude_of_origin",0],'
              '    PARAMETER["central_meridian",-63],'
              '    PARAMETER["scale_factor",0.9996],'
              '    PARAMETER["false_easting",500000],'
              '    PARAMETER["false_northing",0],'
              '    AUTHORITY["EPSG","2038"],'
              '    AXIS["Easting",EAST],'
              '    AXIS["Northing",NORTH]]')

    input2 = ('PROJCS["NAD83(CSRS98) / UTM zone 20N (deprecated)",'
              '    GEOGCS["NAD83(CSRS98)",'
              '        DATUM["NAD83_Canadian_Spatial_Reference_System",'
              '            SPHEROID["GRS 1980",6378137,298.257222101,'
              '                AUTHORITY["EPSG","7019"]],'
              '            TOWGS84[0,0,0,0,0,0,0],'
              '            AUTHORITY["EPSG","6140"]],'
              '        PRIMEM["Greenwich",0,'
              '            AUTHORITY["EPSG","8901"]],'
              '        AUTHORITY["EPSG","4140"]],'
              '    UNIT["metre",1,'
              '        AUTHORITY["EPSG","9001"]],'
              '    PROJECTION["Transverse_Mercator"],'
              '    PARAMETER["central_meridian",-63],'
              '    PARAMETER["scale_factor",0.9996],'
              '    PARAMETER["false_easting",500000],'
              '    PARAMETER["false_northing",0],'
              '    AUTHORITY["EPSG","2038"],'
              '    AXIS["Easting",EAST],'
              '    AXIS["Northing",NORTH]]')

    srs1.SetFromUserInput(input1)
    srs2.SetFromUserInput(input2)
    self.assertTrue(srs1.IsSame(srs2))

  def testBasic12UrnOgrCrs84(self):
    self.assertEqual(osr.GetUserInputAsWKT('CRS:84'),
                     osr.GetUserInputAsWKT('WGS84'))

  # TODO(schwehr): What is wrong with this test?
  @unittest.skip('Fix this test')
  def testBasic13GeoccsLookupInData(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4328)
    wkt = srs.ExportToWkt()
    expected_wkt = (
        'GEOCCS["WGS 84 (geocentric)",DATUM["WGS_1984",SPHEROID["WGS 84",'
        '6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG",'
        '"6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Geocentric X",'
        'OTHER],AXIS["Geocentric Y",OTHER],AXIS["Geocentric Z",NORTH],'
        'AUTHORITY["EPSG","4328"]]')

    self.assertEqual(wkt, expected_wkt)
    self.assertTrue(srs.IsGeocentric())
    self.assertEqual(srs.Validate(), 0)

  def testBasic14ManualSimpleGeocentricWgs84(self):
    srs = osr.SpatialReference()
    srs.SetGeocCS('My Geocentric')
    srs.SetWellKnownGeogCS('WGS84')
    srs.SetLinearUnits('meter', 1.0)
    wkt = srs.ExportToWkt()
    expected_wkt = (
        'GEOCCS["My Geocentric",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,'
        '298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["meter",1]]')

    self.assertEqual(wkt, expected_wkt)
    self.assertTrue(srs.IsGeocentric())
    self.assertEqual(srs.Validate(), 0)

  def testBasic15ValidationAndFixupMethods(self):
    wkt = (
        'GEOCCS["WGS 84 (geocentric)",'
        '    PRIMEM["Greenwich",0,'
        '        AUTHORITY["EPSG","8901"]],'
        '    DATUM["WGS_1984",'
        '        SPHEROID["WGS 84",6378137,298.257223563,'
        '            AUTHORITY["EPSG","7030"]],'
        '        AUTHORITY["EPSG","6326"]],'
        '    AXIS["Geocentric X",OTHER],'
        '    AXIS["Geocentric Y",OTHER],'
        '    AXIS["Geocentric Z",OTHER],'
        '    AUTHORITY["EPSG","4328"]]'
    )

    srs = osr.SpatialReference()
    srs.SetFromUserInput(wkt)
    self.assertNotEqual(srs.Validate(), 0)

    srs.Fixup()
    self.assertEqual(srs.Validate(), 0)

  def testBasic16OsrSetGeocCs(self):
    wkt = (
        'GEOCCS["WGS 84 (geocentric)",'
        '    PRIMEM["Greenwich",0,'
        '        AUTHORITY["EPSG","8901"]],'
        '    DATUM["WGS_1984",'
        '        SPHEROID["WGS 84",6378137,298.257223563,'
        '            AUTHORITY["EPSG","7030"]],'
        '        AUTHORITY["EPSG","6326"]],'
        '    AXIS["Geocentric X",OTHER],'
        '    AXIS["Geocentric Y",OTHER],'
        '    AXIS["Geocentric Z",OTHER],'
        '    AUTHORITY["EPSG","4328"]]'
    )

    wkt_expect = '\n'.join((
        'GEOCCS["a",',
        '    PRIMEM["Greenwich",0,',
        '        AUTHORITY["EPSG","8901"]],',
        '    DATUM["WGS_1984",',
        '        SPHEROID["WGS 84",6378137,298.257223563,',
        '            AUTHORITY["EPSG","7030"]],',
        '        AUTHORITY["EPSG","6326"]],',
        '    AXIS["Geocentric X",OTHER],',
        '    AXIS["Geocentric Y",OTHER],',
        '    AXIS["Geocentric Z",OTHER],',
        '    AUTHORITY["EPSG","4328"]]'
    ))

    srs = osr.SpatialReference()
    srs.SetFromUserInput(wkt)
    srs.SetGeocCS('a')
    wkt_out = srs.ExportToPrettyWkt()
    self.assertEqual(wkt_out, wkt_expect)

  def testBasic16bExpectErrorCannotWorkOnProjcs(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    self.assertNotEqual(srs.SetGeocCS('a'), 0)

  def testBasic16cGeocccsFromInvalid(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput('GEOGCS["foo"]')
    srs.SetGeocCS('bar')
    expect_wkt = 'GEOCCS["bar"]'
    self.assertEqual(srs.ExportToPrettyWkt(), expect_wkt)

  def testBasic17OgcUrl(self):
    wkt1 = osr.GetUserInputAsWKT('urn:ogc:def:crs:EPSG::4326')
    wkt2 = osr.GetUserInputAsWKT('http://www.opengis.net/def/crs/EPSG/0/4326')
    self.assertEqual(wkt1, wkt2)

  def testBasic18OgcUrlCompoundCrs(self):
    url = (
        'http://www.opengis.net/def/crs-compound?'
        '1=http://www.opengis.net/def/crs/EPSG/0/4326'
        '&2=http://www.opengis.net/def/crs/EPSG/0/4326')
    wkt = osr.GetUserInputAsWKT(url)
    self.assertIn('COMPD_CS', wkt)


if __name__ == '__main__':
  unittest.main()
