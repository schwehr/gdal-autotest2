#!/usr/bin/env python
# Copyright 2017 Google Inc. All Rights Reserved.
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
# Copyright (c) 2008, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at mines-paris dot org>
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

"""Tests for the GRIB driver.

Format is described here:

http://www.gdal.org/frmt_grib.html

Rewrite of grib.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/grib.py
"""

import os


from osgeo import gdal
import unittest
from autotest2.gdrivers import gdrivers_util

DRIVER = 'grib'
EXT = '.grb'


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class GribTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(GribTest, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testOpen(self):
    # Upstream grib_1.
    self.CheckDriver()
    filepath = self.getTestFilePath('ds.mint.bin')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((-7127137, 2500, 0, 2105561, 0, -2500))
    wkt = """PROJCS["unnamed",
    GEOGCS["Coordinate System imported from GRIB file",
        DATUM["unknown",
            SPHEROID["Sphere",6371200,0]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.0174532925199433]],
    PROJECTION["Mercator_1SP"],
    PARAMETER["latitude_of_origin",20],
    PARAMETER["central_meridian",0],
    PARAMETER["scale_factor",1],
    PARAMETER["false_easting",0],
    PARAMETER["false_northing",0]]"""
    self.CheckProjection(wkt)
    self.CheckShape(177, 129, 2)
    self.CheckBand(1, 46650, gdal.GDT_Float64, 9999, 13.24999390, 24.9500061035)
    self.CheckBand(2, 46927, gdal.GDT_Float64, 9999, 14.95000610, 24.9500061035)

  def testGrib1(self):
    # Upstream grib_2.
    filepath = self.getTestFilePath('Sample_QuikSCAT.grb')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((-20.2465, 0.333, 0, 56.0505, 0, -0.333))
    wkt = """GEOGCS["Coordinate System imported from GRIB file",
    DATUM["unknown",
        SPHEROID["Sphere",6371200,0]],
    PRIMEM["Greenwich",0],
    UNIT["degree",0.0174532925199433]]"""
    self.CheckProjection(wkt)
    self.CheckShape(66, 74, 4)
    self.CheckBand(1, 35740, gdal.GDT_Float64, 9999, 0, 1)
    self.CheckBand(2, 24744, gdal.GDT_Float64, 9999, -16.415, 6.299)
    self.CheckBand(3, 40666, gdal.GDT_Float64, 9999, -11.679, 13.66)
    self.CheckBand(4, 50714, gdal.GDT_Float64, 9.999e+20, 19598, 25986)

  # TODO(schwehr): Rewrite grib_3.
  # TODO(schwehr): Rewrite grib_4.
  # TODO(schwehr): Rewrite grib_5.
  # TODO(schwehr): Rewrite grib_6.
  # TODO(schwehr): Rewrite grib_7.
  # TODO(schwehr): Rewrite grib_8.
  # TODO(schwehr): Rewrite grib_9.
  # TODO(schwehr): Rewrite grib_10.


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestReadGrib(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestReadGrib, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self):
    self.CheckDriver()

  def testInfo(self):
    filepath = self.getTestFilePath('ds.mint.bin')
    self.CheckOpen(filepath)
    self.CheckInfo()

    for base in ['bug3246', 'Sample_QuikSCAT']:
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()

    filepath = self.getTestFilePath('one_one.grib2')
    self.CheckOpen(filepath)
    self.CheckInfo()

    # TODO(schwehr): MRMS_EchoTop_18_00.50_20161015-133230.grib2
    # Needs GDAL updates to read.

if __name__ == '__main__':
  unittest.main()
