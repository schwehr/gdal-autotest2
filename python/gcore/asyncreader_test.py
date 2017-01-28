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
# Copyright (c) 2010, Even Rouault <even dot rouault at mines dash paris dot org>
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

"""Test async reader.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/asyncreader.py
"""

import contextlib
import unittest


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


@contextlib.contextmanager
def AsyncReader(src, xoff, yoff, xsize, ysize, buf=None, buf_xsize=None,
                buf_ysize=None, buf_type=None, band_list=None, options=None):
  options = options or []
  asyncreader = src.BeginAsyncReader(xoff, yoff, xsize, ysize, buf, buf_xsize,
                                     buf_ysize, buf_type, band_list, options)
  yield asyncreader
  src.EndAsyncReader(asyncreader)


class AsyncReaderTest(unittest.TestCase):

  def testAsyncReader(self):
    filepath = gcore_util.GetTestFilePath('rgbsmall.tif')

    src = gdal.Open(filepath)
    x_size = src.RasterXSize
    y_size = src.RasterYSize
    bands = src.RasterCount
    asyncreader = src.BeginAsyncReader(0, 0, src.RasterXSize, src.RasterYSize)
    buf = asyncreader.GetBuffer()

    self.assertEqual(asyncreader.GetNextUpdatedRegion(0),
                     [gdal.GARIO_COMPLETE, 0, 0, x_size, y_size])

    src.EndAsyncReader(asyncreader)

    expected = [src.GetRasterBand(i).Checksum() for i in range(1, bands + 1)]
    asyncreader = None
    src = None

    drv = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    dst = drv.Create('/vsimem/asyncresult.tif', x_size, y_size, bands)
    dst.WriteRaster(0, 0, x_size, y_size, buf)

    checksum = [dst.GetRasterBand(i).Checksum() for i in range(1, bands + 1)]
    dst = None
    gdal.Unlink('/vsimem/asyncresult.tif')

    self.assertEqual(checksum, expected)

  def testAsyncReaderContextManager(self):
    filepath = gcore_util.GetTestFilePath('rgbsmall.tif')

    src = gdal.Open(filepath)
    x_size = src.RasterXSize
    y_size = src.RasterYSize
    with AsyncReader(src, 0, 0, x_size, y_size) as asyncreader:
      self.assertEqual(asyncreader.GetNextUpdatedRegion(0),
                       [gdal.GARIO_COMPLETE, 0, 0, x_size, y_size])


if __name__ == '__main__':
  unittest.main()
