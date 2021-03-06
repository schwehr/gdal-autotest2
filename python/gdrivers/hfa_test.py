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

"""Test hfa reading.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_read.py
"""

import os

from osgeo import gdal
import unittest
from autotest2.gdrivers import gdrivers_util

EXT = '.img'
DRIVER = gdrivers_util.HFA_DRIVER


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class HfaReadTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(HfaReadTest, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDrvOpenAndBand(self):
    self.CheckDriver()
    band_num = 1
    tests = (
        ('byte.img', 4672, gdal.GDT_Byte),
        ('int16.img', 4672, gdal.GDT_Int16),
        ('uint16.img', 4672, gdal.GDT_UInt16),
        ('int32.img', 4672, gdal.GDT_Int32),
        ('uint32.img', 4672, gdal.GDT_UInt32),
        ('float32.img', 4672, gdal.GDT_Float32),
        ('float64.img', 4672, gdal.GDT_Float64),
        ('utmsmall.img', 50054, gdal.GDT_Byte),
        ('2bit_compressed.img', 11918, gdal.GDT_Byte)
        )
    for filename, checksum, band_type in tests:
      filepath = self.getTestFilePath(filename)
      self.CheckOpen(filepath)
      self.CheckBand(band_num, checksum, band_type)

  def testInfo(self):
    for base in ('byte', 'int16', 'uint16', 'int32',
                 'uint32', 'float32', 'float64', 'utmsmall',
                 '2bit_compressed',
                 'rat', 'small_ov', 'stats_signed_byte',
                 'spill'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()


if __name__ == '__main__':
  unittest.main()
