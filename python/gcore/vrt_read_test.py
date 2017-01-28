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

"""Test vrt reading.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/vrt_read.py
"""
import struct
import unittest


from osgeo import gdal
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

try:
  import numpy
except ImportError:
  numpy = None

EXT = '.vrt'


# All the VRTs reference tif images
@gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
@gdrivers_util.SkipIfDriverMissing(gdrivers_util.VRT_DRIVER)
class VrtReadTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(VrtReadTest, self).setUp(gdrivers_util.VRT_DRIVER, EXT)

  def testVrt00DrvOpenAndBand(self):
    self.CheckDriver()
    tests = (
        ('byte.vrt', 1, 4672, gdal.GDT_Byte),
        ('int16.vrt', 1, 4672, gdal.GDT_Int16),
        ('uint16.vrt', 1, 4672, gdal.GDT_UInt16),
        ('int32.vrt', 1, 4672, gdal.GDT_Int32),
        ('uint32.vrt', 1, 4672, gdal.GDT_UInt32),
        ('float32.vrt', 1, 4672, gdal.GDT_Float32),
        ('float64.vrt', 1, 4672, gdal.GDT_Float64),
        ('cint16.vrt', 1, 5028, gdal.GDT_CInt16),
        ('cint32.vrt', 1, 5028, gdal.GDT_CInt32),
        ('cfloat32.vrt', 1, 5028, gdal.GDT_CFloat32),
        ('cfloat64.vrt', 1, 5028, gdal.GDT_CFloat64),
        ('msubwinbyte.vrt', 2, 2699, gdal.GDT_Byte),
        ('utmsmall.vrt', 1, 50054, gdal.GDT_Byte),
        ('byte_nearest_50pct.vrt', 1, 1192, gdal.GDT_Byte),
        ('byte_averaged_50pct.vrt', 1, 1152, gdal.GDT_Byte),
        ('byte_nearest_200pct.vrt', 1, 18784, gdal.GDT_Byte),
        ('byte_averaged_200pct.vrt', 1, 18784, gdal.GDT_Byte)
        )
    for filename, band_num, checksum, band_type in tests:
      filepath = gcore_util.GetTestFilePath(filename)
      self.CheckOpen(filepath)
      self.CheckBand(band_num, checksum, band_type)

  def testVrt01NonExistingTif(self):
    filepath = gcore_util.GetTestFilePath('idontexist.vrt')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      gdal.ErrorReset()
      src = gdal.Open(filepath)
      self.assertIsNone(src)
      err_msg = gdal.GetLastErrorMsg()
      self.assertIn('idontexist.tif', err_msg)
      self.assertIn('does not exist in the file system', err_msg)

  def testVrt02NonExistingTifProxyPoolAPI(self):
    # http://trac.osgeo.org/gdal/ticket/2837
    filepath = gcore_util.GetTestFilePath('idontexist2.vrt')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      src = gdal.Open(filepath)
      self.assertIsNotNone(src)
      gdal.ErrorReset()
      self.assertEqual(src.GetRasterBand(1).Checksum(), 0)
      self.assertIn('Checksum', gdal.GetLastErrorMsg())
      self.assertIn('read error', gdal.GetLastErrorMsg())
    self.assertEqual(src.GetMetadata(), {})
    self.assertEqual(src.GetRasterBand(1).GetMetadata(), {})
    self.assertEqual(src.GetGCPs(), ())

  # TODO(schwehr): Make sure libzip support is enabled.
  @unittest.skipIf(not numpy, 'Test requires numpy')
  def testVrt03InitBandDataCascadedVrt(self):
    # http://trac.osgeo.org/gdal/ticket/2867
    # Create is not part of this test.  Removed the create tif part.
    filepath = gcore_util.GetTestFilePath('test_mosaic.vrt')
    self.CheckOpen(filepath)

    data = self.src.GetRasterBand(1).ReadRaster(90, 0, 20, 100)
    got = struct.unpack('B' * 20*100, data)
    for i in range(100):
      self.assertEqual(got[i*20 + 9], 255)

  # TODO(schwehr): wrap vsimem creation in a context manager
  @unittest.skipIf(not numpy, 'Requires numpy')
  def testVrt04ComplexSourceAndData(self):
    # http://trac.osgeo.org/gdal/ticket/3977
    drv = gdal.GetDriverByName('GTiff')
    filepath = '/vsimem/test.tif'
    dst = drv.Create(filepath, 1, 1, 1, gdal.GDT_CFloat32)
    data = numpy.array([[1.+3.j]], dtype=numpy.complex64)
    dst.GetRasterBand(1).WriteArray(data)
    dst = None

    vrt_xml = (
        '<VRTDataset rasterXSize="1" rasterYSize="1">'
        '  <VRTRasterBand dataType="CFloat32" band="1">'
        '    <ComplexSource>'
        '      <SourceFilename relativeToVRT="1">/vsimem/test.tif'
        '</SourceFilename>'
        '      <SourceBand>1</SourceBand>'
        '      <ScaleOffset>3</ScaleOffset>'
        '      <ScaleRatio>2</ScaleRatio>'
        '    </ComplexSource>'
        '  </VRTRasterBand>'
        '</VRTDataset>'
        )

    # Surprise!  You can open a vrt by just passing the xml.
    src = gdal.Open(vrt_xml)
    src_data = src.GetRasterBand(1).ReadAsArray()

    gdal.Unlink(filepath)

    self.assertAlmostEqual(src_data[0, 0], 5+9j)

  # TODO(schwehr): context manager for vsimem object
  def testVrt05SerializationBandMetadata(self):
    # http://trac.osgeo.org/gdal/ticket/5326
    with gdrivers_util.ConfigOption('GDAL_PAM_ENABLED', 'ON'):
      src_filepath = gcore_util.GetTestFilePath('testserialization.asc')
      src = gdal.Open(src_filepath)
      drv = gdal.GetDriverByName(gdrivers_util.VRT_DRIVER)
      filepath = '/vsimem/vrt_read_5.vrt'
      # pylint:disable=unused-variable
      dst = drv.CreateCopy(filepath, src)
      src = None
      dst = None

      src = gdal.Open(filepath)
      gcps = src.GetGCPs()
      self.assertEqual(len(gcps), 2)
      self.assertEqual(src.GetGCPCount(), 2)

      self.assertIn('WGS 84', src.GetGCPProjection())
      band = src.GetRasterBand(1)
      self.assertEqual(band.GetDescription(), 'MyDescription')
      self.assertEqual(band.GetUnitType(), 'MyUnit')
      self.assertEqual(band.GetOffset(), 1)
      self.assertEqual(band.GetScale(), 2)
      self.assertEqual(band.GetRasterColorInterpretation(),
                       gdal.GCI_PaletteIndex)
      self.assertEqual(band.GetCategoryNames(), ['Cat1', 'Cat2'])
      ct = band.GetColorTable()
      self.assertEqual(ct.GetColorEntry(0), (0, 0, 0, 255))
      self.assertEqual(ct.GetColorEntry(1), (1, 1, 1, 255))

      self.assertEqual(band.GetMaximum(), 0)
      self.assertEqual(band.GetMinimum(), 2)

    expected_metadata = {
        'STATISTICS_MEAN': '1',
        'STATISTICS_MINIMUM': '2',
        'STATISTICS_MAXIMUM': '0',
        'STATISTICS_STDDEV': '3'}
    self.assertEqual(band.GetMetadata(), expected_metadata)

    src = None
    gdal.Unlink(filepath)


if __name__ == '__main__':
  unittest.main()
