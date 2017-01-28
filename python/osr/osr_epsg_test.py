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
# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test EPSG lookup.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_epsg.py
"""
import unittest


from osgeo import osr
import unittest


class OsrEpsg(unittest.TestCase):

  def testEpsg01(self):
    # EPSG:26591 picks up entry from pcs.override.csv with the
    # adjusted centeral_meridian.
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(26591)

    self.assertEqual(srs.GetProjParm('central_meridian'), -3.45233333333333)

  def testEpsg02Towgs84(self):
    # Values set properly from gcs.override.csv.
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4312)

    self.assertEqual(float(srs.GetAttrValue('TOWGS84', 6)), 2.4232)

  def testEpsg03Towgs84(self):
    # Based on Pulvoko 1942 have the towgs84 values set properly.
    # http://trac.osgeo.org/gdal/ticket/3579
    expected_towgs84 = [33.4, -146.6, -76.3, -0.359, -0.053, 0.844, -0.84]

    for epsg in [3120, 2172, 2173, 2174, 2175, 3333, 3334, 3335, 3329, 3330,
                 3331, 3332, 3328, 4179]:
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(epsg)

      for i in range(6):
        self.assertEqual(float(srs.GetAttrValue('TOWGS84', i)),
                         expected_towgs84[i])

  def testEpsg04Epsg4326NotLatLong(self):
    # http://trac.osgeo.org/gdal/ticket/3813
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)

    self.assertFalse(srs.EPSGTreatsAsLatLong())
    self.assertNotIn('AXIS', srs.ExportToWkt())

  def testEpsg05Epsga4326IsLatLong(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSGA(4326)

    self.assertTrue(srs.EPSGTreatsAsLatLong())
    self.assertIn('AXIS', srs.ExportToWkt())

  def testEpsg06DatumShiftOsgb36(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4277)

    self.assertIn('TOWGS84[446.448,-125.157,542.06,0.15,0.247,0.842,-20.489]',
                  srs.ExportToWkt())

if __name__ == '__main__':
  unittest.main()
