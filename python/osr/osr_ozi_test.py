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
# Copyright (c) 2012, Even Rouault <even dot rouault at mines dash paris dot org>
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

"""Test OZI projection and datum support.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_ozi.py
"""
import unittest


from osgeo import osr
import unittest


class OsrOzi(unittest.TestCase):

  def testOzi01Wgs84(self):
    srs = osr.SpatialReference()
    srs.ImportFromOzi([
        'OziExplorer Map Data File Version 2.2', 'Test_Map', 'Test_Map.png',
        '1 ,Map Code,', 'WGS 84,WGS 84,   0.0000,   0.0000,WGS 84',
        'Map Projection,Lambert Conformal Conic,PolyCal,No,'
        'AutoCalOnly,No,BSBUseWPX,No', 'Projection Setup,     '
        '4.000000000,    10.000000000,,,,    40.000000000,    56.000000000,,,'
    ])

    expected_srs = osr.SpatialReference(
        'PROJCS["unnamed",'
        '    GEOGCS["WGS 84",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS 84",6378137,298.257223563,'
        '                AUTHORITY["EPSG","7030"]],'
        '            AUTHORITY["EPSG","6326"]],'
        '        PRIMEM["Greenwich",0,'
        '            AUTHORITY["EPSG","8901"]],'
        '        UNIT["degree",0.0174532925199433,'
        '            AUTHORITY["EPSG","9122"]],'
        '        AUTHORITY["EPSG","4326"]],'
        '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
        '    PARAMETER["standard_parallel_1",40],'
        '    PARAMETER["standard_parallel_2",56],'
        '    PARAMETER["latitude_of_origin",4],'
        '    PARAMETER["central_meridian",10],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["Meter",1]]')

    self.assertTrue(srs.IsSame(expected_srs))

  def testOzi02Epsg4301(self):
    srs = osr.SpatialReference()
    srs.ImportFromOzi([
        'OziExplorer Map Data File Version 2.2', 'Test_Map', 'Test_Map.png',
        '1 ,Map Code,', 'Tokyo,', 'Map Projection,Latitude/Longitude,,,,,,',
        'Projection Setup,,,,,,,,,,'
    ])

    expected_srs = osr.SpatialReference()
    expected_srs.ImportFromEPSG(4301)

    self.assertTrue(srs.IsSame(expected_srs))

  def testOzi03UnknownEpsg(self):
    srs = osr.SpatialReference()
    srs.ImportFromOzi([
        'OziExplorer Map Data File Version 2.2', 'Test_Map', 'Test_Map.png',
        '1 ,Map Code,', 'European 1950 (Mean France),',
        'Map Projection,Latitude/Longitude,,,,,,', 'Projection Setup,,,,,,,,,,'
    ])

    expected_srs = osr.SpatialReference(
        'GEOGCS["European 1950 (Mean France)",'
        '    DATUM["European 1950 (Mean France)",'
        '        SPHEROID["International 1924",6378388,297],'
        '        TOWGS84[-87,-96,-120,0,0,0,0]],'
        '    PRIMEM["Greenwich",0],'
        '    UNIT["degree",0.0174532925199433]]')

    self.assertTrue(srs.IsSame(expected_srs))

if __name__ == '__main__':
  unittest.main()
