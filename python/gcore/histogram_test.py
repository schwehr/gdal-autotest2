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
# Copyright (c) 2008, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test GetHistogram and GetDefaultHistogram methods.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/histogram.py
"""


import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
class HistogramTiffTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(HistogramTiffTest, self).setUp(gdrivers_util.GTIFF_DRIVER, '.tif')

  def testHist1Simple(self):
    filepath = gcore_util.GetTestFilePath('utmsmall.tif')
    self.CheckOpen(filepath)

    expected_hist = [
        2, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 23, 0, 0, 0, 0, 0, 0,
        0, 0, 29, 0, 0, 0, 0, 0, 0, 0, 46, 0, 0, 0, 0, 0, 0, 0, 69, 0, 0, 0,
        0, 0, 0, 0, 99, 0, 0, 0, 0, 0, 0, 0, 0, 120, 0, 0, 0, 0, 0, 0, 0, 178,
        0, 0, 0, 0, 0, 0, 0, 193, 0, 0, 0, 0, 0, 0, 0, 212, 0, 0, 0, 0, 0, 0,
        0, 281, 0, 0, 0, 0, 0, 0, 0, 0, 365, 0, 0, 0, 0, 0, 0, 0, 460, 0, 0,
        0, 0, 0, 0, 0, 533, 0, 0, 0, 0, 0, 0, 0, 544, 0, 0, 0, 0, 0, 0, 0, 0,
        626, 0, 0, 0, 0, 0, 0, 0, 653, 0, 0, 0, 0, 0, 0, 0, 673, 0, 0, 0, 0,
        0, 0, 0, 629, 0, 0, 0, 0, 0, 0, 0, 0, 586, 0, 0, 0, 0, 0, 0, 0, 541,
        0, 0, 0, 0, 0, 0, 0, 435, 0, 0, 0, 0, 0, 0, 0, 348, 0, 0, 0, 0, 0, 0,
        0, 341, 0, 0, 0, 0, 0, 0, 0, 0, 284, 0, 0, 0, 0, 0, 0, 0, 225, 0, 0,
        0, 0, 0, 0, 0, 237, 0, 0, 0, 0, 0, 0, 0, 172, 0, 0, 0, 0, 0, 0, 0, 0,
        159, 0, 0, 0, 0, 0, 0, 0, 105, 0, 0, 0, 0, 0, 0, 0, 824
    ]
    band = self.src.GetRasterBand(1)

    self.assertEqual(band.GetHistogram(), expected_hist)

  def testHist2Buckets(self):
    filepath = gcore_util.GetTestFilePath('utmsmall.tif')
    self.CheckOpen(filepath)

    expected_hist = [
        10, 52, 115, 219, 371, 493, 825, 1077, 1279, 1302, 1127, 783, 625, 462,
        331, 929
    ]
    band = self.src.GetRasterBand(1)

    self.assertEqual(band.GetHistogram(buckets=16, max=255.5, min=-0.5,
                                       include_out_of_range=True,
                                       approx_ok=False),
                     expected_hist)

  def testHist5GetDefaultHistogram(self):
    filepath = gcore_util.GetTestFilePath('utmsmall.tif')
    self.CheckOpen(filepath)

    expected_hist = (
        -0.5, 255.5, 256,
        [
            2, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 23, 0, 0, 0, 0, 0,
            0, 0, 0, 29, 0, 0, 0, 0, 0, 0, 0, 46, 0, 0, 0, 0, 0, 0, 0, 69, 0,
            0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0, 0, 0, 0, 0, 120, 0, 0, 0, 0, 0,
            0, 0, 178, 0, 0, 0, 0, 0, 0, 0, 193, 0, 0, 0, 0, 0, 0, 0, 212, 0,
            0, 0, 0, 0, 0, 0, 281, 0, 0, 0, 0, 0, 0, 0, 0, 365, 0, 0, 0, 0, 0,
            0, 0, 460, 0, 0, 0, 0, 0, 0, 0, 533, 0, 0, 0, 0, 0, 0, 0, 544, 0,
            0, 0, 0, 0, 0, 0, 0, 626, 0, 0, 0, 0, 0, 0, 0, 653, 0, 0, 0, 0, 0,
            0, 0, 673, 0, 0, 0, 0, 0, 0, 0, 629, 0, 0, 0, 0, 0, 0, 0, 0, 586,
            0, 0, 0, 0, 0, 0, 0, 541, 0, 0, 0, 0, 0, 0, 0, 435, 0, 0, 0, 0, 0,
            0, 0, 348, 0, 0, 0, 0, 0, 0, 0, 341, 0, 0, 0, 0, 0, 0, 0, 0, 284,
            0, 0, 0, 0, 0, 0, 0, 225, 0, 0, 0, 0, 0, 0, 0, 237, 0, 0, 0, 0, 0,
            0, 0, 172, 0, 0, 0, 0, 0, 0, 0, 0, 159, 0, 0, 0, 0, 0, 0, 0, 105,
            0, 0, 0, 0, 0, 0, 0, 824
            ])
    band = self.src.GetRasterBand(1)
    self.assertEqual(band.GetDefaultHistogram(force=True), expected_hist)


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.AAIGRID_DRIVER)
class HistogramAaiGridTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(HistogramAaiGridTest, self).setUp(gdrivers_util.AAIGRID_DRIVER,
                                            '.grd')

  def testHist3AaiGrid(self):
    filepath = gcore_util.GetTestFilePath('int32_withneg.grd')
    self.CheckOpen(filepath)

    expected_hist = [
        0, 0, 0, 0, 0, 1, 0, 1, 1, 3, 3, 2, 0, 5, 3, 4, 0, 1, 1, 2, 3
        ]
    band = self.src.GetRasterBand(1)

    self.assertEqual(band.GetHistogram(buckets=21, max=100, min=-100,
                                       include_out_of_range=True,
                                       approx_ok=False),
                     expected_hist)

  def testHist4AaiGridDoNotIncludeOutOfRange(self):
    filepath = gcore_util.GetTestFilePath('int32_withneg.grd')
    self.CheckOpen(filepath)

    expected_hist = [
        0, 0, 0, 0, 0, 1, 0, 1, 1, 3, 3, 2, 0, 5, 3, 4, 0, 1, 1, 2, 0
        ]
    band = self.src.GetRasterBand(1)

    self.assertEqual(band.GetHistogram(buckets=21, max=100, min=-100,
                                       include_out_of_range=False,
                                       approx_ok=False),
                     expected_hist)


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.JPEG_DRIVER)
class HistogramJpegTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(HistogramJpegTest, self).setUp(gdrivers_util.JPEG_DRIVER, '.grd')

  def testHist6Jpeg(self):
    # http://trac.osgeo.org/gdal/ticket/3304
    filepath = gdrivers_util.GetTestFilePath('jpeg/albania.jpg')
    self.CheckOpen(filepath)

    band = self.src.GetRasterBand(1)

    self.assertIsNone(band.GetDefaultHistogram(force=False))


if __name__ == '__main__':
  unittest.main()
