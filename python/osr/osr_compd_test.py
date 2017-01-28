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
# Copyright (c) 2010, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test of COMPD_CS support.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_compd.py
"""

import unittest


from osgeo import osr
import unittest

# For test 1 and 2.
COMPD_WKT = (
    'COMPD_CS["OSGB36 / British National Grid + ODN",PROJCS["OSGB 1936 / '
    'British National Grid",GEOGCS["OSGB 1936",DATUM["OSGB_1936",'
    'SPHEROID["Airy 1830",6377563.396,299.3249646,AUTHORITY["EPSG",7001]],'
    'TOWGS84[375,-111,431,0,0,0,0],AUTHORITY["EPSG",6277]],PRIMEM["Greenwich",'
    '0,AUTHORITY["EPSG",8901]],UNIT["DMSH",0.0174532925199433,AUTHORITY["EPSG",'
    '9108]],AXIS["Lat",NORTH],AXIS["Long",EAST],AUTHORITY["EPSG",4277]],'
    'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",49],'
    'PARAMETER["central_meridian",-2],PARAMETER["scale_factor",0.999601272],'
    'PARAMETER["false_easting",400000],PARAMETER["false_northing",-100000],'
    'UNIT["metre_1",1,AUTHORITY["EPSG",9001]],AXIS["E",EAST],AXIS["N",NORTH],'
    'AUTHORITY["EPSG",27700]],VERT_CS["Newlyn",VERT_DATUM["Ordnance Datum '
    'Newlyn",2005,AUTHORITY["EPSG",5101]],UNIT["metre_2",1,AUTHORITY["EPSG",'
    '9001]],AXIS["Up",UP],AUTHORITY["EPSG",5701]],AUTHORITY["EPSG",7405]]'
)


class OsrCompd(unittest.TestCase):

  def CheckSrsAgainstWkt(self, srs, expected_wkt):
    self.assertEqual(srs.Validate(), 0)
    srs_expected = osr.SpatialReference()
    srs_expected.ImportFromWkt(expected_wkt)
    self.assertTrue(srs.IsSame(srs_expected))
    wkt = srs.ExportToPrettyWkt()
    self.assertEqual(wkt, expected_wkt)

  def testCompd01(self):
    srs = osr.SpatialReference()
    srs.ImportFromWkt(COMPD_WKT)
    self.assertTrue(srs.IsProjected())
    self.assertFalse(srs.IsGeographic())
    self.assertFalse(srs.IsLocal())
    self.assertTrue(srs.IsCompound())

    expected_proj4 = (
        '+proj=tmerc +lat_0=49 +lon_0=-2 +k=0.999601272 +x_0=400000 '
        '+y_0=-100000 +ellps=airy +towgs84=375,-111,431,0,0,0,0 '
        '+units=m +vunits=m +no_defs '
        )

    result_proj4 = srs.ExportToProj4()
    self.assertEqual(result_proj4, expected_proj4)

  def testCompd02SetFromUserInput(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(COMPD_WKT)
    self.assertEqual(srs.Validate(), 0)
    self.assertTrue(srs.IsProjected())

  # TODO(schwehr): What is wrong with this test?
  @unittest.skip('Fix this test')
  def testCompd03Expansion(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(7401)
    expected_wkt = '\n'.join((
        'COMPD_CS["NTF (Paris) / France II + NGF Lallemand",',
        '    PROJCS["NTF (Paris) / France II (deprecated)",',
        '        GEOGCS["NTF (Paris)",',
        '            DATUM["Nouvelle_Triangulation_Francaise_Paris",',
        ('                SPHEROID["Clarke 1880 (IGN)",6378249.2,'
         '293.4660212936265,'),
        '                    AUTHORITY["EPSG","7011"]],',
        '                TOWGS84[-168,-60,320,0,0,0,0],',
        '                AUTHORITY["EPSG","6807"]],',
        '            PRIMEM["Paris",2.33722917,',
        '                AUTHORITY["EPSG","8903"]],',
        '            UNIT["grad",0.01570796326794897,',
        '                AUTHORITY["EPSG","9105"]],',
        '            AUTHORITY["EPSG","4807"]],',
        '        PROJECTION["Lambert_Conformal_Conic_1SP"],',
        '        PARAMETER["latitude_of_origin",52],',
        '        PARAMETER["central_meridian",0],',
        '        PARAMETER["scale_factor",0.99987742],',
        '        PARAMETER["false_easting",600000],',
        '        PARAMETER["false_northing",2200000],',
        '        UNIT["metre",1,',
        '            AUTHORITY["EPSG","9001"]],',
        '        AXIS["X",EAST],',
        '        AXIS["Y",NORTH],',
        '        AUTHORITY["EPSG","27582"]],',
        '    VERT_CS["NGF Lallemand height",',
        ('        VERT_DATUM["Nivellement General de la France - Lallemand",'
         '2005,'),
        '            AUTHORITY["EPSG","5118"]],',
        '        UNIT["metre",1,',
        '            AUTHORITY["EPSG","9001"]],',
        '        AXIS["Up",UP],',
        '        AUTHORITY["EPSG","5719"]],',
        '    AUTHORITY["EPSG","7401"]]'
        ))

    self.CheckSrsAgainstWkt(srs, expected_wkt)

  # TODO(schwehr): What is wrong with this test?
  @unittest.skip('Fix this test')
  def testCompd04ExpansionGcsVertCs(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(7400)
    expected_wkt = '\n'.join((
        'COMPD_CS["NTF (Paris) + NGF IGN69 height",',
        '    GEOGCS["NTF (Paris)",',
        '        DATUM["Nouvelle_Triangulation_Francaise_Paris",',
        '            SPHEROID["Clarke 1880 (IGN)",6378249.2,293.4660212936265,',
        '                AUTHORITY["EPSG","7011"]],',
        '            TOWGS84[-168,-60,320,0,0,0,0],',
        '            AUTHORITY["EPSG","6807"]],',
        '        PRIMEM["Paris",2.33722917,',
        '            AUTHORITY["EPSG","8903"]],',
        '        UNIT["grad",0.01570796326794897,',
        '            AUTHORITY["EPSG","9105"]],',
        '        AUTHORITY["EPSG","4807"]],',
        '    VERT_CS["NGF-IGN69 height",',
        '        VERT_DATUM["Nivellement General de la France - IGN69",2005,',
        '            AUTHORITY["EPSG","5119"]],',
        '        UNIT["metre",1,',
        '            AUTHORITY["EPSG","9001"]],',
        '        AXIS["Up",UP],',
        '        AUTHORITY["EPSG","5720"]],',
        '    AUTHORITY["EPSG","7400"]]'
        ))

    self.CheckSrsAgainstWkt(srs, expected_wkt)

  # TODO(schwehr): What is wrong with this test?
  @unittest.skip('Fix this test')
  def testCompd05GridShiftFilesAndProj4(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput('EPSG:26911+5703')
    expected_wkt = '\n'.join((
        'COMPD_CS["NAD83 / UTM zone 11N + NAVD88 height",',
        '    PROJCS["NAD83 / UTM zone 11N",',
        '        GEOGCS["NAD83",',
        '            DATUM["North_American_Datum_1983",',
        '                SPHEROID["GRS 1980",6378137,298.257222101,',
        '                    AUTHORITY["EPSG","7019"]],',
        '                TOWGS84[0,0,0,0,0,0,0],',
        '                AUTHORITY["EPSG","6269"]],',
        '            PRIMEM["Greenwich",0,',
        '                AUTHORITY["EPSG","8901"]],',
        '            UNIT["degree",0.0174532925199433,',
        '                AUTHORITY["EPSG","9122"]],',
        '            AUTHORITY["EPSG","4269"]],',
        '        PROJECTION["Transverse_Mercator"],',
        '        PARAMETER["latitude_of_origin",0],',
        '        PARAMETER["central_meridian",-117],',
        '        PARAMETER["scale_factor",0.9996],',
        '        PARAMETER["false_easting",500000],',
        '        PARAMETER["false_northing",0],',
        '        UNIT["metre",1,',
        '            AUTHORITY["EPSG","9001"]],',
        '        AXIS["Easting",EAST],',
        '        AXIS["Northing",NORTH],',
        '        AUTHORITY["EPSG","26911"]],',
        '    VERT_CS["NAVD88 height",',
        '        VERT_DATUM["North American Vertical Datum 1988",2005,',
        '            AUTHORITY["EPSG","5103"],',
        ('            EXTENSION["PROJ4_GRIDS","g2012a_conus.gtx,'
         'g2012a_alaska.gtx,g2012a_guam.gtx,g2012a_hawaii.gtx,'
         'g2012a_puertorico.gtx,g2012a_samoa.gtx"]],'),
        '        UNIT["metre",1,',
        '            AUTHORITY["EPSG","9001"]],',
        '        AXIS["Up",UP],',
        '        AUTHORITY["EPSG","5703"]]]'
        ))
    self.CheckSrsAgainstWkt(srs, expected_wkt)

    exp_proj4 = (
        '+proj=utm +zone=11 +ellps=GRS80 +towgs84=0,0,0,0,0,0,0 +units=m '
        '+geoidgrids=g2012a_conus.gtx,g2012a_alaska.gtx,g2012a_guam.gtx,'
        'g2012a_hawaii.gtx,g2012a_puertorico.gtx,g2012a_samoa.gtx '
        '+vunits=m +no_defs '
        )
    self.assertEqual(srs.ExportToProj4(), exp_proj4)

  # TODO(schwehr): Skip if not proj4 >= 4.8.0.
  def testCompd06ConvertFromProj4WithVertUnits(self):
    # TODO(schwehr): Implement this test.
    pass

  # TODO(schwehr): What is wrong with this test?
  @unittest.skip('Fix this test')
  def testCompd07SetCompound(self):
    srs_horiz = osr.SpatialReference()
    srs_horiz.ImportFromEPSG(4326)

    srs_vert = osr.SpatialReference()
    srs_vert.ImportFromEPSG(5703)
    srs_vert.SetTargetLinearUnits('VERT_CS', 'foot', 0.304800609601219)

    srs = osr.SpatialReference()
    srs.SetCompoundCS('My Compound SRS', srs_horiz, srs_vert)

    expected_wkt = '\n'.join((
        'COMPD_CS["My Compound SRS",',
        '    GEOGCS["WGS 84",',
        '        DATUM["WGS_1984",',
        '            SPHEROID["WGS 84",6378137,298.257223563,',
        '                AUTHORITY["EPSG","7030"]],',
        '            AUTHORITY["EPSG","6326"]],',
        '        PRIMEM["Greenwich",0,',
        '            AUTHORITY["EPSG","8901"]],',
        '        UNIT["degree",0.0174532925199433,',
        '            AUTHORITY["EPSG","9122"]],',
        '        AUTHORITY["EPSG","4326"]],',
        '    VERT_CS["NAVD88 height",',
        '        VERT_DATUM["North American Vertical Datum 1988",2005,',
        '            AUTHORITY["EPSG","5103"],',
        ('            EXTENSION["PROJ4_GRIDS","g2012a_conus.gtx,'
         'g2012a_alaska.gtx,g2012a_guam.gtx,g2012a_hawaii.gtx,'
         'g2012a_puertorico.gtx,g2012a_samoa.gtx"]],'),
        '        UNIT["foot",0.304800609601219],',
        '        AXIS["Up",UP],',
        '        AUTHORITY["EPSG","5703"]]]'
        ))

    self.CheckSrsAgainstWkt(srs, expected_wkt)

  # TODO(schwehr): What is wrong with this test?
  @unittest.skip('Fix this test')
  def testCompd08ImportFromUrn(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput('urn:ogc:def:crs,crs:EPSG::27700,crs:EPSG::5701')
    self.assertEqual(srs.Validate(), 0)
    self.assertIn('COMPD_CS', srs.ExportToWkt())


if __name__ == '__main__':
  unittest.main()
