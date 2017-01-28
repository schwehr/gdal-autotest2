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
# Copyright (c) 2009, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test RasterAttributeTables.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/rat.py
"""

import os
import unittest


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


class RatTest(unittest.TestCase):

  def setUp(self):
    rat = gdal.RasterAttributeTable()

    rat.CreateColumn('Value', gdal.GFT_Integer, gdal.GFU_MinMax)
    rat.CreateColumn('Count', gdal.GFT_Integer, gdal.GFU_PixelCount)

    rat.SetRowCount(3)

    rat.SetValueAsInt(0, 0, 10)
    rat.SetValueAsInt(0, 1, 100)
    rat.SetValueAsInt(1, 0, 11)
    rat.SetValueAsInt(1, 1, 200)
    rat.SetValueAsInt(2, 0, 12)
    rat.SetValueAsInt(2, 1, 90)

    self.rat = rat

  def CheckRat(self, rat):
    self.assertEqual(rat.GetColumnCount(), 2)
    self.assertEqual(rat.GetRowCount(), 3)
    self.assertEqual(rat.GetNameOfCol(0), 'Value')
    self.assertEqual(rat.GetNameOfCol(1), 'Count')
    self.assertEqual(rat.GetUsageOfCol(1), gdal.GFU_PixelCount)
    self.assertEqual(rat.GetTypeOfCol(1), gdal.GFT_Integer)
    self.assertEqual(rat.GetRowOfValue(11.0), 1)
    self.assertEqual(rat.GetValueAsInt(1, 1), 200)

  def testRat01Clone(self):
    rat = self.rat.Clone()
    self.CheckRat(rat)

  @unittest.skip('Needs gdal > 1.10.0')
  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.PNM_DRIVER)
  def testRat02PnmPlusAuxXml(self):
    with gdrivers_util.ConfigOption('GDAL_PAM_ENABLED', 'ON'):
      with gcore_util.TestTemporaryDirectory() as tmpdir:
        tmp_filepath = os.path.join(tmpdir, 'rat2.pnm')
        drv = gdal.GetDriverByName('PNM')

        # TODO(schwehr): Use context manager.
        dst = drv.Create(tmp_filepath, 10, 10, 1, gdal.GDT_Byte)
        self.CheckRat(self.rat)
        dst.GetRasterBand(1).SetDefaultRAT(self.rat)
        dst = None

        src = gdal.Open(tmp_filepath, gdal.GA_Update)
        rat = src.GetRasterBand(1).GetDefaultRAT()
        self.CheckRat(rat)
        src.GetRasterBand(1).SetDefaultRAT(None)
        src = None

        src = gdal.Open(tmp_filepath)
        self.assertIsNotNone(src.GetRasterBand(1).GetDefaultRAT())


if __name__ == '__main__':
  unittest.main()
