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

"""Tests for the kakadu driver.

Format is described here:

http://www.gdal.org/frmt_jp2kak.html

Rewrite of jp2kak.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/jp2kak.py
"""

import os


from osgeo import gdal
from osgeo import osr
import unittest
from autotest2.gdrivers import gdrivers_util
from autotest2.gdrivers import jp2k_util

DRIVER = 'jp2kak'
EXT = '.jp2'


def SrsFromEpsg(epsg):
  srs = osr.SpatialReference()
  srs.ImportFromEPSG(epsg)
  return srs


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.JP2KAK_DRIVER)
class Jp2kakTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(Jp2kakTest, self).setUp(gdrivers_util.JP2KAK_DRIVER, EXT)

  def testOpen(self):
    with jp2k_util.DeregisterJpeg2000(skip=gdrivers_util.JP2KAK_DRIVER):
      self.CheckDriver()
      files = (
          ('byte.jp2', 50054, gdal.GDT_Byte),
          ('int16.jp2', 4587, gdal.GDT_Int16))

      for filename, checksum, gdal_type in files:
        filepath = gdrivers_util.GetTestFilePath(filename)
        self.CheckOpen(filepath)
        self.CheckBand(1, checksum, gdal_type)
        self.CheckCreateCopy(
            check_checksums=True,
            check_stats=True,
            options=['QUALITY=100'],
            vsimem=True)

  def testCheckCopyDefaultQuality(self):
    filepath = gdrivers_util.GetTestFilePath('rgbsmall.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.assertEqual(self.src.RasterCount, 3)
    self.CheckBand(1, 21212, gdal.GDT_Byte)
    self.CheckBand(2, 21053, gdal.GDT_Byte)
    self.CheckBand(3, 21349, gdal.GDT_Byte)

    checksums = [22046, 22696, 23327]
    stats = [(0.0, 228.0), (0.0, 230.0), (0.0, 184)]

    # The options correspond to jp2kak_4 and jp2_kak5 in autotest.
    for options in ([], ['GMLJP2=OFF'], ['GEOJP2=OFF']):
      self.CheckCreateCopy(
          check_checksums=True,
          check_stats=True,
          options=options,
          vsimem=True,
          checksums=checksums,
          stats=stats)

  # No tests 6 and 7 currently exist in upstream.

  # TODO(schwehr): Rewrite test case jp2kak_8.

  def testReadYCbCrColorModel(self):
    # Covers upstream tests jp2kak_9 and jp2kak_10.
    filename = 'rgbwcmyk01_YeGeo_kakadu.jp2'
    filepath = gdrivers_util.GetTestFilePath(os.path.join('jpeg2k', filename))
    self.CheckOpen(filepath)
    self.CheckGeoTransform((270000, 240, 0, 4336500, 0, -240))
    self.CheckProjection(SrsFromEpsg(26918).ExportToWkt())
    self.CheckBand(1, 32136, gdal.GDT_Byte)
    self.CheckBand(2, 32141, gdal.GDT_Byte)
    self.CheckBand(3, 32126, gdal.GDT_Byte)

    # Spot check DirectRasterIO() C++ method.
    data = self.src.ReadRaster(0, 0, 800, 100, band_list=[2, 3])

    expected = [(0, 0), (255, 0), (0, 255), (255, 255), (255, 255), (0, 255),
                (255, 0), (0, 0)]

    result = [(ord(data[i * 100]), ord(data[80000 + i * 100]))
              for i in range(8)]
    self.assertEqual(expected, result)

  def testRead11BitSigned(self):
    # Matches upstream jp2kak_11.
    filename = 'gtsmall_11_int16.jp2'
    filepath = gdrivers_util.GetTestFilePath(os.path.join('jpeg2k', filename))
    self.CheckOpen(filepath)
    self.CheckGeoTransform((-100, 0.00833333, 0, 40, 0, -0.00833333))
    self.CheckProjection(SrsFromEpsg(4326).ExportToWkt())
    self.CheckBand(1, 63474, gdal.GDT_Int16)

  def testRead10BitUnsigned(self):
    # Matches upstream jp2kak_12.
    filename = 'gtsmall_10_uint16.jp2'
    filepath = gdrivers_util.GetTestFilePath(os.path.join('jpeg2k', filename))
    self.CheckOpen(filepath)
    self.CheckGeoTransform((-100, 0.00833333, 0, 40, 0, -0.00833333))
    self.CheckProjection(SrsFromEpsg(4326).ExportToWkt())
    self.CheckBand(1, 63354, gdal.GDT_UInt16)

  # TODO(schwehr): Rewrite test case jp2kak_13.
  #   Requires PCIDSK support that is not currently available.
  # TODO(schwehr): Rewrite test case jp2kak_14.
  #   Requires PCIDSK support that is not currently available.

  def testResolutionInfo(self):
    # Matches upstream jp2kak_15 and jp2kak_16.
    filename = 'small_200ppcm.jp2'
    filepath = gdrivers_util.GetTestFilePath(os.path.join('jpeg2k', filename))
    self.CheckOpen(filepath)
    self.CheckGeoTransform((271500, 307.08661417, 0, 4335000, 0, -306.28272251))
    self.CheckProjection(SrsFromEpsg(26918).ExportToWkt())
    self.CheckBand(1, 12650, gdal.GDT_Byte)
    self.CheckBand(2, 12650, gdal.GDT_Byte)
    self.CheckBand(3, 12650, gdal.GDT_Byte)

    metadata = self.src.GetMetadata()
    expected_metadata = {
        'Corder': 'PCRL',
        'TIFFTAG_RESOLUTIONUNIT': '3 (pixels/cm)',
        'TIFFTAG_XRESOLUTION': '200.012',
        'TIFFTAG_YRESOLUTION': '200.012'
    }
    self.assertEqual(expected_metadata, metadata)

    self.CheckCreateCopy(
        check_checksums=True,
        check_stats=True,
        vsimem=True,
        metadata=expected_metadata)

  def testAlternateAxisOrder(self):
    # Matches upstream jp2kak_17.
    filename = 'gmljp2_dtedsm_epsg_4326_axes_alt_offsetVector.jp2'
    filepath = gdrivers_util.GetTestFilePath(os.path.join('jpeg2k', filename))
    with gdrivers_util.ConfigOption('GDAL_JP2K_ALT_OFFSETVECTOR_ORDER', 'YES'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform((42.99958, 0.0082713, 0, 34.000417, 0, -0.0082713))
      self.CheckProjection(SrsFromEpsg(4326).ExportToWkt())
      self.CheckBand(1, 39998, gdal.GDT_Int16)
    with gdrivers_util.ConfigOption('GDAL_JP2K_ALT_OFFSETVECTOR_ORDER', 'NO'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform((42.99958, 0, 0.0082713, 34.000417, -0.0082713, 0))
      self.CheckProjection(SrsFromEpsg(4326).ExportToWkt())
      self.CheckBand(1, 39998, gdal.GDT_Int16)

  def testLosslessCopyInt16(self):
    # Matches upstream jp2kak_18 and jp2kak_19.
    tests = (('int16.tif', gdal.GDT_Int16), ('uint16.tif', gdal.GDT_UInt16))
    for filename, gdal_type in tests:
      filepath = gdrivers_util.GetTestFilePath(filename)
      self.CheckOpen(filepath, check_driver=False)
      self.CheckBand(1, 4672, gdal_type)
      self.CheckCreateCopy(
          check_checksums=True,
          check_stats=True,
          options=['QUALITY=100'],
          vsimem=True)

  def test1BitTo8BitPromotion(self):
    filename = 'stefan_full_rgba_alpha_1bit.jp2'
    filepath = gdrivers_util.GetTestFilePath(os.path.join('jpeg2k', filename))
    self.CheckOpen(filepath)
    self.CheckBand(1, 17530, gdal.GDT_Byte)
    self.CheckBand(2, 65071, gdal.GDT_Byte)
    self.CheckBand(3, 42900, gdal.GDT_Byte)
    self.CheckBand(4, 8527, gdal.GDT_Byte)

    # TODO(schwehr): Rewrite the rest of test case jp2kak_20.

  # TODO(schwehr): Rewrite test case jp2kak_21.


if __name__ == '__main__':
  unittest.main()
