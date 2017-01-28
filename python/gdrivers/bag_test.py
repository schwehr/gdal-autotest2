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
# Copyright (c) 2010-2013, Even Rouault <even . rouault at mines-paris dot org>
#                     Frank Warmerdam <warmerdam@pobox.com>
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
"""Test the Bathymetry Attributed Grid (BAG) driver.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/bagf.py
"""

import unittest

from osgeo import gdal
from autotest2.gdrivers import gdrivers_util

EXT = '.bag'


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.BAG_DRIVER)
class BagTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(BagTest, self).setUp(gdrivers_util.BAG_DRIVER, EXT)
    self.dst = None

  def testBag02TrueNorthNominal(self):
    filepath = gdrivers_util.GetTestFilePath('true_n_nominal.bag')
    self.CheckOpen(filepath)
    for band_num, checksum, nodata in ((1, 1072, 1e6), (2, 150, 1e6),
                                       (3, 1315, 1e6)):
      self.CheckBand(band_num, checksum, gdal.GDT_Float32, nodata)

    band1 = self.src.GetRasterBand(1)
    self.assertAlmostEqual(10, band1.GetMinimum())
    self.assertAlmostEqual(19.8, band1.GetMaximum(), delta=0.000001)

    xml_bag = self.src.GetMetadata('xml:BAG')[0]
    self.assertIn('<?xml', xml_bag)

    # TODO(schwehr): Do we need to have the check for closing the file?

  def testBag03SouthernHemisphereFalseNorthing(self):
    filepath = gdrivers_util.GetTestFilePath('southern_hemi_false_northing.bag')
    self.CheckOpen(filepath)

    self.assertEqual(self.src.RasterCount, 2)
    for band_num, checksum, nodata in ((1, 21402, 1e6), (2, 33216, 1e6)):
      self.CheckBand(band_num, checksum, gdal.GDT_Float32, nodata)

    geotransform = (615037.5, 75.0, 0.0, 9559387.5, 0.0, -75.0)
    self.CheckGeoTransform(geotransform)

    self.CheckProjection(
        'PROJCS["UTM Zone 13, Southern Hemisphere",'
        '    GEOGCS["WGS 84",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS 84",6378137,298.257223563,'
        '                AUTHORITY["EPSG","7030"]],'
        '            TOWGS84[0,0,0,0,0,0,0],'
        '            AUTHORITY["EPSG","6326"]],'
        '        PRIMEM["Greenwich",0,'
        '            AUTHORITY["EPSG","8901"]],'
        '        UNIT["degree",0.0174532925199433,'
        '            AUTHORITY["EPSG","9108"]],'
        '        AUTHORITY["EPSG","4326"]],'
        '    PROJECTION["Transverse_Mercator"],'
        '    PARAMETER["latitude_of_origin",0],'
        '    PARAMETER["central_meridian",-105],'
        '    PARAMETER["scale_factor",0.9996],'
        '    PARAMETER["false_easting",500000],'
        '    PARAMETER["false_northing",10000000],'
        '    UNIT["Meter",1]]'
    )

  # TODO(schwehr): Test BAG version 1.5.


if __name__ == '__main__':
  unittest.main()
