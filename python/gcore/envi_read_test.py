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
# Copyright (c) 2003, Andrey Kiselev <dron@remotesensing.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

"""Test envi reading.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/envi_read.py
"""


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


EXT = '.raw'


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.ENVI_DRIVER)
class EnviReadTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(EnviReadTest, self).setUp(gdrivers_util.ENVI_DRIVER, EXT)

  def testDrvOpenAndBand(self):
    self.CheckDriver()
    band_num = 1
    # All tests also requires an hdr file for the .raw.
    tests = (
        ('byte.raw', 4672, gdal.GDT_Byte),
        ('int16.raw', 4672, gdal.GDT_Int16),
        ('uint16.raw', 4672, gdal.GDT_UInt16),
        ('int32.raw', 4672, gdal.GDT_Int32),
        ('uint32.raw', 4672, gdal.GDT_UInt32),
        ('float32.raw', 4672, gdal.GDT_Float32),
        ('float64.raw', 4672, gdal.GDT_Float64),
        ('cfloat32.raw', 5028, gdal.GDT_CFloat32),
        ('cfloat64.raw', 5028, gdal.GDT_CFloat64)
        )
    for filename, checksum, band_type in tests:
      filepath = gcore_util.GetTestFilePath(filename)
      self.CheckOpen(filepath)
      self.CheckBand(band_num, checksum, band_type)


if __name__ == '__main__':
  unittest.main()
