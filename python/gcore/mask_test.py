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

"""Test RFC 15 mask band.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/mask.py
"""

import os


from osgeo import gdal
from osgeo import gdalconst
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

EXT = '.tif'


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
class MaskTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(MaskTest, self).setUp(gdrivers_util.GTIFF_DRIVER, EXT)

  def testMask01AllValidChecksum(self):
    filepath = gcore_util.GetTestFilePath('byte.tif')
    self.CheckOpen(filepath)
    band = self.src.GetRasterBand(1)
    self.assertEqual(band.GetMaskFlags(),
                     gdalconst.GMF_ALL_VALID)
    self.assertEqual(band.GetMaskBand().Checksum(), 4873)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.VRT_DRIVER)
  def testMask02VrtNodata(self):
    filepath = gcore_util.GetTestFilePath('byte.vrt')
    self.CheckOpen(filepath, check_driver='vrt')
    band = self.src.GetRasterBand(1)
    self.assertEqual(band.GetMaskFlags(),
                     gdalconst.GMF_NODATA)
    self.assertEqual(band.GetMaskBand().Checksum(), 4209)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.PNG_DRIVER)
  def testMask03PngAlpha(self):
    filepath = gcore_util.GetTestFilePath('stefan_full_rgba.png')
    self.CheckOpen(filepath, check_driver='png')
    expected_flags = gdalconst.GMF_ALPHA + gdalconst.GMF_PER_DATASET

    band_1 = self.src.GetRasterBand(1)
    band_2 = self.src.GetRasterBand(2)
    band_3 = self.src.GetRasterBand(3)
    band_4 = self.src.GetRasterBand(4)

    self.assertEqual(band_1.GetMaskFlags(), expected_flags)
    self.assertEqual(band_1.GetMaskBand().Checksum(), 10807)

    self.assertEqual(band_2.GetMaskFlags(), expected_flags)
    self.assertEqual(band_3.GetMaskFlags(), expected_flags)

    # Alpha channel.
    self.assertEqual(band_4.GetMaskFlags(), gdal.GMF_ALL_VALID)
    self.assertEqual(band_4.GetMaskBand().Checksum(), 36074)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.JPEG_DRIVER)
  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.PNM_DRIVER)
  def testMask04Jpg(self):
    filepath = gdrivers_util.GetTestFilePath('masked.jpg')
    # TODO(schwehr): Set check_driver to 'jpeg'.
    self.CheckOpen(filepath, check_driver=False)
    band_src = self.src.GetRasterBand(1)
    self.assertEqual(band_src.GetMaskFlags(), gdal.GMF_PER_DATASET)
    self.assertEqual(band_src.GetMaskBand().Checksum(), 770)

    with gdrivers_util.ConfigOption('GDAL_PAM_ENABLED', 'ON'):
      with gcore_util.TestTemporaryDirectory(prefix='MaskCopy') as tmpdir:
        filepath_dst = os.path.join(tmpdir, 'mask_4.pnm')
        drv = gdal.GetDriverByName(gdrivers_util.PNM_DRIVER)
        dst = drv.CreateCopy(filepath_dst, self.src)
        band = dst.GetRasterBand(1)
        self.assertEqual(band.GetMaskFlags(), gdal.GMF_PER_DATASET)
        self.assertEqual(band.GetMaskBand().Checksum(), 770)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.JPEG_DRIVER)
  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.PNM_DRIVER)
  def testMask05Overviews(self):
    # Will fail with libtiff 3.8.2.
    metadata = gdal.GetDriverByName('GTiff').GetMetadata()
    self.assertIn('BigTIFF', metadata['DMD_CREATIONOPTIONLIST'])

    filepath = gdrivers_util.GetTestFilePath('masked.jpg')
    # TODO(schwehr): Set check_driver to 'jpeg'.
    self.CheckOpen(filepath, check_driver=False)
    with gdrivers_util.ConfigOption('GDAL_PAM_ENABLED', 'ON'):
      with gcore_util.TestTemporaryDirectory(prefix='MaskOverview') as tmpdir:
        filepath_dst = os.path.join(tmpdir, 'mask_4.pnm')
        drv = gdal.GetDriverByName(gdrivers_util.PNM_DRIVER)
        dst = drv.CreateCopy(filepath_dst, self.src)
        dst = None
        dst = gdal.Open(filepath_dst, gdal.GA_Update)
        dst.BuildOverviews(overviewlist=[2, 4])
        overview = dst.GetRasterBand(1).GetOverview(1)
        self.assertEqual(overview.GetMaskFlags(), gdal.GMF_PER_DATASET)

# TODO(schwehr): Implement tests 6 through 23.

if __name__ == '__main__':
  unittest.main()
