#!/usr/bin/env python

# Copyright 2016 Google Inc. All Rights Reserved.
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
# Copyright (c) 2008-2014, Even Rouault <even dot rouault at mines-paris.org>
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
###############################################################################

"""Test the JPEG JFIF raster driver in gdal.

Format is described here:

http://www.gdal.org/frmt_jpeg.html

Rewrite of jpeg.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/jpeg.py
"""

import os


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

DRIVER = gdrivers_util.JPEG_DRIVER
EXT = '.jpg'


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class JpegTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(JpegTest, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testJpegSimpleRead1(self):
    self.CheckDriver()
    filepath = self.getTestFilePath('albania.jpg')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((0.0, 1.0, 0.0, 0.0, 0.0, 1.0))
    prj_expected = ''  # No projection.
    self.CheckProjection(prj_expected)
    self.CheckBand(1, 61830, gdal.GDT_Byte, None)
    self.CheckBand(2, 17016, gdal.GDT_Byte, None)
    self.CheckBand(3, 20715, gdal.GDT_Byte, None)

  def testMetadataExifFilelist2(self):
    filepath = self.getTestFilePath('albania.jpg')
    self.CheckOpen(filepath)

    expected_metadata = {
        'EXIF_DateTimeDigitized': '    :  :     :  :  ',
        'EXIF_DateTimeOriginal': '    :  :     :  :  ',
        'EXIF_ExifVersion': '0210',
        'EXIF_FlashpixVersion': '0100',
        'EXIF_GPSLatitude': '(41) (1) (22.91)',
        'EXIF_GPSLatitudeRef': 'N',
        'EXIF_GPSLongitude': '(19) (55) (42.35)',
        'EXIF_GPSLongitudeRef': 'E',
        'EXIF_GPSVersionID': '0x2 00 00 00',
        'EXIF_Make': '',
        'EXIF_Model': '',
        'EXIF_PixelXDimension': '361',
        'EXIF_PixelYDimension': '260',
        'EXIF_ResolutionUnit': '2',
        'EXIF_XResolution': '(96)',
        'EXIF_YResolution': '(96)'}

    self.assertEqual(self.src.GetMetadata(), expected_metadata)

    expected_structure = {
        'COMPRESSION': 'JPEG',
        'INTERLEAVE': 'PIXEL',
        'SOURCE_COLOR_SPACE': 'YCbCr'}

    self.assertEqual(self.src.GetMetadata('IMAGE_STRUCTURE'),
                     expected_structure)

    file_list = self.src.GetFileList()
    self.assertEqual(len(file_list), 1)
    self.assertEqual(os.path.basename(file_list[0]), 'albania.jpg')

  # TODO(schwehr): Port tests 3-25.

  def testFailCreateCopyOnEmpty26(self):
    src = gdal.GetDriverByName('Mem').Create('', 70000, 1)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      dst = self.driver.CreateCopy('/vsimem/jpeg_26.jpg', src)
      self.assertIsNone(dst)

  def testInfo(self):
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      for base in ('albania', 'black_with_white_exif_ovr', 'byte_with_xmp',
                   'masked', 'rgb_ntf_cmyk', 'rgbsmall_rgb', 'small_world',
                   'vophead'):
        filepath = self.getTestFilePath(base + EXT)
        self.CheckOpen(filepath)
        self.CheckInfo()

if __name__ == '__main__':
  unittest.main()
