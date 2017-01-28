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

"""Test the NITF driver.

Format is described here:

http://www.gdal.org/frmt_nitf.html

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/nitf.py
"""
import array
import os
import shutil
import struct
import unittest

from osgeo import gdal

from osgeo import gdal
from osgeo import osr
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

EXT = '.ntf'


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.NITF_DRIVER)
class NitfTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(NitfTest, self).setUp(gdrivers_util.NITF_DRIVER, EXT)
    self.dst = None

  def tearDown(self):
    if self.dst:
      filelist = self.dst.GetFileList() or []
      for dst_filepath in filelist:
        gdal.Unlink(dst_filepath)

  def CheckCreateNitf(self, filepath, options=None, inverted_color_interp=True):
    options = options or []
    xsize = 200
    ysize = 100
    num_bands = 3
    dst = self.driver.Create(filepath, xsize, ysize, num_bands, gdal.GDT_Byte,
                             options)
    geotransform = (100, 0.1, 0.0, 30.0, 0.0, -0.1)
    dst.SetGeoTransform(geotransform)
    if inverted_color_interp:
      band_interps = (gdal.GCI_BlueBand, gdal.GCI_GreenBand, gdal.GCI_RedBand)
    else:
      band_interps = (gdal.GCI_RedBand, gdal.GCI_GreenBand, gdal.GCI_BlueBand)
    for band_num, band_interp in zip(range(1, 4), band_interps):
      dst.GetRasterBand(band_num).SetRasterColorInterpretation(band_interp)

    raw_data = array.array('h', (list(range(200)) + list(range(20, 220)) +
                                 list(range(30, 230)))).tostring()
    for line in range(100):
      dst.WriteRaster(0, line, xsize, 1, raw_data,
                      buf_type=gdal.GDT_Int16,
                      band_list=[1, 2, 3])
    expected_checksums = [dst.GetRasterBand(i).Checksum() for i in range(1, 4)]
    dst = None

    self.CheckOpen(filepath)
    for band_num, checksum in zip(range(1, 4), expected_checksums):
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)
    self.CheckGeoTransform(geotransform)

    for band_num, band_interp in zip(range(1, 4), band_interps):
      band = self.src.GetRasterBand(band_num)
      self.assertEqual(band.GetRasterColorInterpretation(), band_interp)

  def testNitf01CreateCopy8bit(self):
    filepath = gdrivers_util.GetTestFilePath('byte.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    # Strict False needed for ERROR 6: NITF only supports WGS84 geographic and
    # UTM.  The lack of support for the projection means it must be turned off.
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf02CreateCopy16bit(self):
    filepath = gdrivers_util.GetTestFilePath('int16.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Same error 6 as in test 1.
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf03CreateCopyWithGeoref(self):
    filepath = gdrivers_util.GetTestFilePath('rgbsmall.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # GeoTransform was pretty far off: -22.932584 != -22.932448980.
      self.dst = self.CheckCreateCopy(check_geotransform=False, vsimem=True)
    self.assertTrue(self.dst)

  def testNitf04CreationIcordsG(self):
    filepath = '/vsimem/test04_create.ntf'
    self.CheckCreateNitf(filepath, options=['ICORDS=G'])

  # Test 5 is now a part of test 4.

  def testNitf06AdjustedIgeoloInterp(self):
    filepath = gdrivers_util.GetTestFilePath('rgb.ntf')
    self.CheckOpen(filepath)
    for band_num, checksum in ((1, 21212), (2, 21053), (3, 21349)):
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)
    self.CheckGeoTransform((-44.842029478458, 0.003503401360, 0,
                            -22.930748299319, 0, -0.003503401360))
    self.CheckProjection('WGS84')

  # Test 7 was create copy in memory.  Already done.

  def testNitf08NsifFileWithBlockaMetadata(self):
    filepath = gdrivers_util.GetTestFilePath('fake_nsif.ntf')
    self.CheckOpen(filepath)
    self.CheckBand(1, 12033, gdal.GDT_Byte)
    self.CheckGeoTransform((20.07498084, 0.00011494, 0.00159003,
                            41.28381944, 0.00118295, -0.00007183))
    self.CheckProjection('WGS84')

    md = self.src.GetMetadata()
    self.assertEqual(md['NITF_FHDR'], 'NSIF01.00')
    self.assertEqual(md['NITF_BLOCKA_BLOCK_INSTANCE_01'], '01')
    self.assertEqual(md['NITF_BLOCKA_BLOCK_COUNT'], '01')
    self.assertEqual(md['NITF_BLOCKA_N_GRAY_01'], '00000')
    self.assertEqual(md['NITF_BLOCKA_L_LINES_01'], '01000')
    self.assertEqual(md['NITF_BLOCKA_LAYOVER_ANGLE_01'], '000')
    self.assertEqual(md['NITF_BLOCKA_SHADOW_ANGLE_01'], '000')
    self.assertEqual(md['NITF_BLOCKA_FRLC_LOC_01'], '+41.319331+020.078400')
    self.assertEqual(md['NITF_BLOCKA_LRLC_LOC_01'], '+41.317083+020.126072')
    self.assertEqual(md['NITF_BLOCKA_LRFC_LOC_01'], '+41.281634+020.122570')
    self.assertEqual(md['NITF_BLOCKA_FRFC_LOC_01'], '+41.283881+020.074924')

  # TODO(schwehr): Write testNitf09JpegEncoded.
  # TODO(schwehr): Write testNitf10.

  def testNitf11OneBit(self):
    # http://trac.osgeo.org/gdal/ticket/1854
    filepath = gdrivers_util.GetTestFilePath('i_3034c.ntf')
    self.CheckOpen(filepath)
    self.CheckBand(1, 170, gdal.GDT_Byte)

  def testNitf12TreAndCgmAccess(self):
    filepath = gdrivers_util.GetTestFilePath('fake_nsif.ntf')
    self.CheckOpen(filepath)

    expected_block_a = (
        '010000001000000000                '
        '+41.319331+020.078400+41.317083+020.126072'
        '+41.281634+020.122570+41.283881+020.074924     '
        )
    self.assertEqual(self.src.GetMetadataItem('BLOCKA', 'TRE'),
                     expected_block_a)
    self.assertEqual(self.src.GetMetadata('TRE')['BLOCKA'], expected_block_a)

    self.assertEqual(self.src.GetMetadataItem('SEGMENT_COUNT', 'CGM'), '0')
    self.assertEqual(self.src.GetMetadata('CGM')['SEGMENT_COUNT'], '0')

  def testNitf13UtmZone11South(self):
    filepath = '/vsimem/test_13.ntf'
    xsize = 200
    ysize = 100
    num_bands = 1
    options = ['ICORDS=S']
    geotransform = (400000, 10, 0, 6000000, 0, -10)
    wkt = (
        'PROJCS["UTM Zone 11, Southern Hemisphere",'
        '    GEOGCS["WGS 84",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS 84",6378137,298.257223563,'
        '                AUTHORITY["EPSG","7030"]],'
        '            TOWGS84[0,0,0,0,0,0,0],'
        '            AUTHORITY["EPSG","6326"]],'
        '        PRIMEM["Greenwich",0,'
        '            AUTHORITY["EPSG","8901"]],'
        '        UNIT["degree",0.0174532925199433,'
        '            AUTHORITY["EPSG","9108"]],'
        '        AUTHORITY["EPSG","4326"]],'
        '    PROJECTION["Transverse_Mercator"],'
        '    PARAMETER["latitude_of_origin",0],'
        '    PARAMETER["central_meridian",-117],'
        '    PARAMETER["scale_factor",0.9996],'
        '    PARAMETER["false_easting",500000],'
        '    PARAMETER["false_northing",10000000],'
        '    UNIT["Meter",1]]'
        )
    dst_tmp = self.driver.Create(filepath, xsize, ysize, num_bands,
                                 gdal.GDT_Byte, options)
    dst_tmp.SetGeoTransform(geotransform)
    dst_tmp.SetProjection(wkt)

    raw_data = array.array('f', range(xsize)).tostring()

    for line in range(100):
      dst_tmp.WriteRaster(0, line, xsize, num_bands, raw_data,
                          buf_type=gdal.GDT_Int16,
                          band_list=[1])
    dst_tmp = None

    self.dst = self.CheckOpen(filepath)
    self.assertTrue(self.dst)
    self.CheckBand(1, 55964, gdal.GDT_Byte)
    self.CheckGeoTransform(geotransform)
    self.CheckProjection(wkt)

  # Test 14 now a part of 13.
  # Test 15 is the same as 1.

  def testNitf16OneMonoWithMask0BlackAsTransparent(self):
    # From:
    # http://www.gwg.nga.mil/ntb/baseline/software/testfile/Nitfv2_1/ns3034d.nsf
    filepath = gdrivers_util.GetTestFilePath('ns3034d.nsf')
    self.CheckOpen(filepath)
    self.CheckBand(1, 170, gdal.GDT_Byte)
    self.dst = self.CheckCreateCopy(vsimem=True)
    self.assertTrue(self.dst)

  def testNitf17OneBitRgbLutMaskTable0Tranparent1ToGreen(self):
    # From:
    # http://www.gwg.nga.mil/ntb/baseline/software/testfile/Nitfv2_1/i_3034f.ntf
    filepath = gdrivers_util.GetTestFilePath('i_3034f.ntf')
    self.CheckOpen(filepath)
    self.CheckBand(1, 170, gdal.GDT_Byte)
    self.dst = self.CheckCreateCopy(vsimem=True)
    self.assertTrue(self.dst)

  def testNitf18WithoutImageSegment(self):
    # From:
    # http://www.gwg.nga.mil/ntb/baseline/software/testfile/Nitfv1_1/U_0006A.NTF
    filepath = gdrivers_util.GetTestFilePath('U_0006A.NTF')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.CheckOpen(filepath)
    self.assertEqual(self.src.RasterCount, 0)

  def testNitf19BilevelC1Decompression(self):
    # From:
    # http://www.gwg.nga.mil/ntb/baseline/software/testfile/Nitfv2_0/U_1050A.NTF
    filepath = gdrivers_util.GetTestFilePath('U_1050A.NTF')
    self.CheckOpen(filepath)
    self.CheckBand(1, 65024, gdal.GDT_Byte)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Warning 1: FHDR=NITF02.00 not supported, switching to NITF02
      self.dst = self.CheckCreateCopy(check_projection=False, vsimem=True)
      self.assertTrue(self.dst)

  def testNitf20OnlyHeader(self):
    # From:
    # http://www.gwg.nga.mil/ntb/baseline/software/testfile/Nitfv1_1/U_0002A.NTF
    filepath = gdrivers_util.GetTestFilePath('U_0002A.NTF')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      src = gdal.Open(filepath)
    self.assertEqual(gdal.GetLastErrorNo(), 6)
    self.assertIn('Unable to read header length from NITF',
                  gdal.GetLastErrorMsg())
    self.assertIsNone(src)

  def testNitf21TextAccessViaMetadataDomain(self):
    filepath = gdrivers_util.GetTestFilePath('ns3114a.nsf')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.CheckOpen(filepath)
    self.assertEqual(gdal.GetLastErrorNo(), 1)
    self.assertIn('NITF file, but no image', gdal.GetLastErrorMsg())
    self.assertIn('blocks were found on it', gdal.GetLastErrorMsg())

    self.assertEqual(self.src.GetMetadataItem('DATA_0', 'TEXT'), 'A')
    self.assertEqual(self.src.GetMetadata('TEXT')['DATA_0'], 'A')

  def testNitf22CreateCopyInt32(self):
    filepath = gcore_util.GetTestFilePath('int32.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    self.CheckBand(1, 4672, gdal.GDT_Int32)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Warning 6: NITF only supports WGS84 geographic and UTM projections.
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf23CreateCopyFloat32(self):
    filepath = gcore_util.GetTestFilePath('float32.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    self.CheckBand(1, 4672, gdal.GDT_Float32)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Warning 6: NITF only supports WGS84 geographic and UTM projections.
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf24CreateCopyFloat64(self):
    filepath = gcore_util.GetTestFilePath('float64.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    self.CheckBand(1, 4672, gdal.GDT_Float64)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Warning 6: NITF only supports WGS84 geographic and UTM projections.
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf25CreateCopyUInt16(self):
    filepath = gcore_util.GetTestFilePath('uint16.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    self.CheckBand(1, 4672, gdal.GDT_UInt16)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Warning 6: NITF only supports WGS84 geographic and UTM projections.
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf26CreateCopyUInt32(self):
    filepath = gcore_util.GetTestFilePath('uint32.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    self.CheckBand(1, 4672, gdal.GDT_UInt32)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      # Warning 6: NITF only supports WGS84 geographic and UTM projections.
      self.dst = self.CheckCreateCopy(check_projection=False, strict=False,
                                      vsimem=True)
    self.assertTrue(self.dst)

  def testNitf27CreateIcNcCompressionAndMultiBlocks(self):
    filepath = '/vsimem/test27_create.ntf'
    options = ['ICORDS=G', 'IC=NC', 'BLOCKXSIZE=10', 'BLOCKYSIZE=10']
    self.CheckCreateNitf(filepath, options=options)

  # TODO(schwehr): Write test 28_jp2ecw.  Check for ECW support.
  # TODO(schwehr): Write test 28_jp2mrsid.  Check for MrSID support.
  # TODO(schwehr): Write test 28_jp2kak.  Check for Kakadu support.

  def testNitf29CreateWithLut(self):
    filepath = '/vsimem/test29_create.ntf'
    dst = self.driver.Create(filepath, 1, 1, 1, gdal.GDT_Byte,
                             ['IREP=RGB/LUT', 'LUT_SIZE=128'])
    ct = gdal.ColorTable()
    color_table = (
        (255, 255, 255, 255),
        (255, 255, 0, 255),
        (255, 0, 255, 255),
        (0, 255, 255, 255)
    )
    for entry_num, values in enumerate(color_table):
      ct.SetColorEntry(entry_num, values)
    dst.GetRasterBand(1).SetRasterColorTable(ct)
    dst = None

    self.CheckOpen(filepath)
    ct = self.src.GetRasterBand(1).GetRasterColorTable()
    self.assertEqual(ct.GetCount(), 129)
    for entry_num, expected_values in enumerate(color_table):
      self.assertEqual(ct.GetColorEntry(entry_num), expected_values)

    try:
      self.dst = self.CheckCreateCopy(vsimem=True)
      ct = self.dst.GetRasterBand(1).GetRasterColorTable()
      # TODO(schwehr): Why did the count go up by one?
      self.assertEqual(ct.GetCount(), 130)
      for entry_num, expected_values in enumerate(color_table):
        self.assertEqual(ct.GetColorEntry(entry_num), expected_values)
    finally:
      # This is happening in memory and there is already a self.dst that
      # will get cleaned up, so cleanup self.src.
      filelist = self.src.GetFileList()
      for src_filepath in filelist:
        gdal.Unlink(src_filepath)

  def testNitf30WriteBlockaTre(self):
    filepath = gdrivers_util.GetTestFilePath('fake_nsif.ntf')
    self.CheckOpen(filepath)
    self.dst = self.CheckCreateCopy(vsimem=True)

    md = self.dst.GetMetadata()
    self.assertEqual(md['NITF_FHDR'], 'NSIF01.00')
    self.assertEqual(md['NITF_BLOCKA_BLOCK_INSTANCE_01'], '01')
    self.assertEqual(md['NITF_BLOCKA_BLOCK_COUNT'], '01')
    self.assertEqual(md['NITF_BLOCKA_N_GRAY_01'], '00000')
    self.assertEqual(md['NITF_BLOCKA_L_LINES_01'], '01000')
    self.assertEqual(md['NITF_BLOCKA_LAYOVER_ANGLE_01'], '000')
    self.assertEqual(md['NITF_BLOCKA_SHADOW_ANGLE_01'], '000')
    self.assertEqual(md['NITF_BLOCKA_FRLC_LOC_01'], '+41.319331+020.078400')
    self.assertEqual(md['NITF_BLOCKA_LRLC_LOC_01'], '+41.317083+020.126072')
    self.assertEqual(md['NITF_BLOCKA_LRFC_LOC_01'], '+41.281634+020.122570')
    self.assertEqual(md['NITF_BLOCKA_FRFC_LOC_01'], '+41.283881+020.074924')

  def testNitf31CustomTre(self):
    values = {'TOTEST': 'SecondTRE', 'CUSTOM': ' Test TRE1\\0MORE'}
    options = ['TRE=%s=%s' % (k, v) for k, v in values.iteritems()]
    options.append('ICORDS=G')
    self.CheckCreateNitf('/vsimem/test31.ntf', options=options)
    self.dst = self.src  # Request cleanup.

    self.assertEqual(self.src.GetMetadata('TRE'), values)
    for key, expected_value in values.iteritems():
      self.assertEqual(self.src.GetMetadataItem(key, 'TRE'), expected_value)
    for band_num, checksum in zip((1, 2, 3), (32498, 42602, 38982)):
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)

  def testNitf32CreateIcordsD(self):
    self.CheckCreateNitf('/vsimem/test32.ntf', options=['ICORDS=D'])
    self.dst = self.src  # Request cleanup.

    for band_num, checksum in zip((1, 2, 3), (32498, 42602, 38982)):
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)

  def testNitf33CreateIcordsD(self):
    options = [
        'ICORDS=D',
        'BLOCKA_BLOCK_COUNT=01',
        'BLOCKA_BLOCK_INSTANCE_01=01',
        'BLOCKA_L_LINES_01=100',
        'BLOCKA_FRLC_LOC_01=+29.950000+119.950000',
        'BLOCKA_LRLC_LOC_01=+20.050000+119.950000',
        'BLOCKA_LRFC_LOC_01=+20.050000+100.050000',
        'BLOCKA_FRFC_LOC_01=+29.950000+100.050000'
    ]
    self.CheckCreateNitf('/vsimem/test33.ntf', options=options)
    self.dst = self.src  # Request cleanup.

    for band_num, checksum in zip((1, 2, 3), (32498, 42602, 38982)):
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)

    md = self.src.GetMetadata()
    for option in options:
      key = 'NITF_' + option.split('=')[0]
      expected_value = option.split('=')[1]
      value = md[key]
      if expected_value != value:
        value = value.lstrip()
      self.assertEqual(value, expected_value)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.DTED_DRIVER)
  def testNitf34CreateCopy16BitWithTiling(self):
    filepath = gdrivers_util.GetTestFilePath('n43.dt0')
    self.CheckOpen(filepath, check_driver='dted')
    self.CheckBand(1, 49187, gdal.GDT_Int16)
    dst = self.CheckCreateCopy('/vsimem/test34.ntf',
                               options=['BLOCKSIZE=64'])
    self.assertEqual(dst.GetRasterBand(1).GetBlockSize(), [64, 64])

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.VRT_DRIVER)
  def testNitf35CreateCopyWithTextSegment(self):
    filepath = gdrivers_util.GetTestFilePath('text_md.vrt')
    self.CheckOpen(filepath, check_driver='vrt')
    dst = self.CheckCreateCopy(vsimem=True)

    text_md = dst.GetMetadata('TEXT')
    self.assertEqual(text_md['DATA_0'], 'This is text data\nwith a newline.')
    self.assertEqual(text_md['DATA_1'],
                     'Also, a second text segment is created.')

  # TODO(schwehr): Write testNitf36JpegEncodedC3WithBlocks.

  def testNitf37CreateWith69999Bands(self):
    filepath = '/vsimem/test37.ntf'
    num_bands = 69999
    dst = self.driver.Create(filepath, 1, 1, num_bands)
    self.assertTrue(dst)
    dst = None

    self.CheckOpen(filepath)
    self.assertEqual(self.src.RasterCount, num_bands)
    self.dst = self.src

  def testNitf38CreateWith999Images(self):
    filepath = gdrivers_util.GetTestFilePath('rgbsmall.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    xsize = self.src.RasterXSize
    ysize = self.src.RasterYSize
    data = self.src.GetRasterBand(1).ReadRaster(0, 0, xsize, ysize)
    options = ['NUMI=999']
    dst_filepath = '/vsimem/test38.nitf'
    dst = self.driver.Create(dst_filepath, xsize, ysize, 1, options=options)
    dst = None

    dst = gdal.Open('NITF_IM:998:' + dst_filepath, gdal.GA_Update)
    dst.GetRasterBand(1).WriteRaster(0, 0, xsize, ysize, data)
    dst.BuildOverviews(overviewlist=[2])
    dst = None

    dst = gdal.Open('NITF_IM:0:' + dst_filepath)
    self.assertEqual(dst.GetRasterBand(1).Checksum(), 0)

    dst = gdal.Open('NITF_IM:998:' + dst_filepath)
    self.assertEqual(dst.GetRasterBand(1).Checksum(),
                     self.src.GetRasterBand(1).Checksum())

  # TODO(schwehr): Test for newer version of libjpeg.
  def testNitf39JpegM3WithSeveralBlocks(self):
    filepath = gdrivers_util.GetTestFilePath('rgbsmall.tif')
    self.CheckOpen(filepath, check_driver=gdrivers_util.GTIFF_DRIVER)
    options = ['IC=M3', 'BLOCKSIZE=32', 'QUALITY=100']
    dst = self.CheckCreateCopy(check_checksums=False, check_geotransform=False,
                               vsimem=True, options=options)
    expected_mean = 65.4208
    expected_stddev = 47.254550335
    mean, stddev = dst.GetRasterBand(1).ComputeBandStats()
    self.assertAlmostEqual(mean, expected_mean, places=1)
    self.assertAlmostEqual(stddev, expected_stddev, places=0)
    self.assertEqual(dst.GetMetadata('IMAGE_STRUCTURE')['COMPRESSION'], 'JPEG')

  # TODO(schwehr): Write testNitf40Large10GB.
  # TODO(schwehr): Write testNitf41Jpeg12Bit.
  # TODO(schwehr): Write testNitf42Jpeg12BitCreateCopy.
  # TODO(schwehr): Write testNitf43Jpeg2000Jasper.
  # TODO(schwehr): Write testNitf43Jpeg2000Ecw.
  # TODO(schwehr): Write testNitf43Jpeg2000Kakadu.

  def testNitf44MonoBlock1000x1Image(self):
    # http://trac.osgeo.org/gdal/ticket/3263
    filepath = '/vsimem/test44.nitf'
    src = self.driver.Create(filepath, 10000, 1)
    src.GetRasterBand(1).Fill(255)
    src = None
    self.CheckOpen(filepath)
    self.assertEqual(self.src.GetRasterBand(1).GetBlockSize(), [10000, 1])
    self.CheckBand(1, 57182, gdal.GDT_Byte)

  def testNitf45OverviewsOnJpegCompressSubdataset(self):
    filepath_orig = gdrivers_util.GetTestFilePath('two_images_jpeg.ntf')
    with gcore_util.TestTemporaryDirectory(prefix='nitf45') as tmpdir:
      filepath = os.path.join(tmpdir, 'nitf45.ntf')
      shutil.copy(filepath_orig, filepath)
      filepath = 'NITF_IM:1:' + filepath
      src = gdal.Open(filepath, gdal.GA_Update)
      src.BuildOverviews(overviewlist=[2])
      src = None

      self.CheckOpen(filepath)
      self.CheckBand(1, 4743, gdal.GDT_Byte)
      band = self.src.GetRasterBand(1)
      self.assertEqual(band.GetOverviewCount(), 1)
      self.assertEqual(band.GetOverview(0).Checksum(), 1086)

  # TODO(schwehr): Write testNit46Jpeg2000CompressedSubdataset.

  def testNitf47Rsets(self):
    filepath = gdrivers_util.GetTestFilePath('rset.ntf.r0')
    self.CheckOpen(filepath)
    self.CheckBand(2, 21053, gdal.GDT_Byte)
    self.assertEqual(self.src.GetRasterBand(2).GetOverviewCount(), 2)
    self.assertEqual(self.src.GetRasterBand(2).GetOverview(1).Checksum(), 1297)

  def testNitf48Overviewsw(self):
    base = 'rset.ntf.r'
    filepath_orig = gdrivers_util.GetTestFilePath(base)
    with gcore_util.TestTemporaryDirectory(prefix='nitf45') as tmpdir:
      for i in range(3):
        shutil.copy(filepath_orig + str(i), os.path.join(tmpdir, base + str(i)))
      filepath = os.path.join(tmpdir, base + '0')
      src = gdal.Open(filepath, gdal.GA_Update)
      src.BuildOverviews(overviewlist=[3])
      src = None

      self.CheckOpen(filepath)
      band = self.src.GetRasterBand(1)
      self.assertEqual(band.GetOverviewCount(), 1)
      self.assertEqual(band.GetOverview(0).Checksum(), 2328)

  def testNitf49TextCgmCreateCopy(self):
    # http://trac.osgeo.org/gdal/ticket/3376
    filepath = gdrivers_util.GetTestFilePath('text_md.vrt')
    self.CheckOpen(filepath, check_driver='vrt')
    # HEADER_0 is invalid, but does not matter.
    options = [
        'TEXT=DATA_0=COUCOU',
        'TEXT=HEADER_0=ABC',
        'CGM=SEGMENT_COUNT=1',
        'CGM=SEGMENT_0_SLOC_ROW=25',
        'CGM=SEGMENT_0_SLOC_COL=25',
        'CGM=SEGMENT_0_SDLVL=2',
        'CGM=SEGMENT_0_SALVL=1',
        'CGM=SEGMENT_0_DATA=XYZ'
        ]
    self.dst = self.CheckCreateCopy(vsimem=True, options=options)
    self.assertEqual(self.dst.GetMetadata('TEXT')['DATA_0'], 'COUCOU')
    cgm = self.dst.GetMetadata('CGM')
    self.assertEqual(cgm['SEGMENT_COUNT'], '1')
    self.assertEqual(cgm['SEGMENT_0_DATA'], 'XYZ')

  def testNitf50TextCgmCopy(self):
    # http://trac.osgeo.org/gdal/ticket/3376
    filepath = gdrivers_util.GetTestFilePath('/vsimem/test_nitf50.ntf')
    # HEADER_0 is invalid, but does not matter.
    options = [
        'TEXT=DATA_0=COUCOU',
        'TEXT=HEADER_0=ABC',
        'CGM=SEGMENT_COUNT=1',
        'CGM=SEGMENT_0_SLOC_ROW=25',
        'CGM=SEGMENT_0_SLOC_COL=25',
        'CGM=SEGMENT_0_SDLVL=2',
        'CGM=SEGMENT_0_SALVL=1',
        'CGM=SEGMENT_0_DATA=XYZ'
        ]
    xsize = 100
    ysize = 100
    num_bands = 3
    src = self.driver.Create(filepath, xsize, ysize, num_bands, options=options)
    # TODO(schwehr): The '   ' looks wrong.  Investigate.
    src.WriteRaster(0, 0, xsize, ysize, '   ', 1, 1,
                    buf_type=gdal.GDT_Byte, band_list=[1, 2, 3])
    band_interps = (gdal.GCI_BlueBand, gdal.GCI_GreenBand, gdal.GCI_RedBand)
    for band_num, color_interp in zip((1, 2, 3), band_interps):
      src.GetRasterBand(band_num).SetRasterColorInterpretation(color_interp)
    src = None

    self.dst = self.CheckOpen(filepath)
    self.assertEqual(self.dst.GetMetadata('TEXT')['DATA_0'], 'COUCOU')
    cgm = self.dst.GetMetadata('CGM')
    self.assertEqual(cgm['SEGMENT_COUNT'], '1')
    self.assertEqual(cgm['SEGMENT_0_DATA'], 'XYZ')

  def testNitf51VerySmallBitsPerPixelLessThan8Or12(self):
    with gcore_util.TestTemporaryDirectory(prefix='nitf51') as tmpdir:

      for xsize in range(1, 9):
        for bits_per_pixel in range(1, 8) + [12]:
          basename = 'test_nitf51_%d_%d.ntf' % (xsize, bits_per_pixel)
          filepath = os.path.join(tmpdir, basename)
          src = self.driver.Create(filepath, xsize, 1)
          self.assertTrue(src)
          src = None

          with open(filepath, 'r+b') as nitf_file:
            # Patch the bits per pixel.
            nitf_file.seek(811)
            data1 = 48 + bits_per_pixel / 10
            data2 = 48 + bits_per_pixel % 10
            nitf_file.write(struct.pack('2B', data1, data2))

            # Write image data.
            nitf_file.seek(843)
            num_bytes = (xsize*bits_per_pixel + 7) / 8
            data = struct.pack('%dB' % num_bytes, *([255,]*num_bytes))

            nitf_file.write(data)

          self.CheckOpen(filepath)
          band = self.src.GetRasterBand(1)
          buf_type = gdal.GDT_UInt16 if bits_per_pixel == 12 else gdal.GDT_Byte
          data = band.ReadRaster(0, 0, xsize, 1, buf_type=buf_type)
          struct_str = ('%d' % xsize) + ('H' if bits_per_pixel == 12 else 'B')
          arr = struct.unpack(struct_str, data)

          expected_value = (1 << bits_per_pixel) - 1
          for i in range(xsize):
            self.assertEqual(arr[i], expected_value)

  def testNitf52GeoSdeTre(self):
    filepath = '/vsimem/test_nitf52.ntf'
    options = [
        'FILE_TRE=GEOPSB=' + 8 * '0123456789' + '012345EURM' + 353 * ' ',
        'FILE_TRE=PRJPSB=' + 8 * '0123456789' + 'AC' + 31 * '0',
        'TRE=MAPLOB=M  0001000010000000000100000000000005000000'
        ]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None

    self.dst = self.CheckOpen(filepath)
    self.CheckProjection(
        'PROJCS["unnamed",'
        '    GEOGCS["EUROPEAN 1950, Mean (3 Param)",'
        '        DATUM["EUROPEAN 1950, Mean (3 Param)",'
        '            SPHEROID["International 1924            ",6378388,297],'
        '            TOWGS84[-87,-98,-121,0,0,0,0]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["degree",0.0174532925199433]],'
        '    PROJECTION["Albers_Conic_Equal_Area"],'
        '    PARAMETER["standard_parallel_1",0],'
        '    PARAMETER["standard_parallel_2",0],'
        '    PARAMETER["latitude_of_center",0],'
        '    PARAMETER["longitude_of_center",0],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0]]'
        )
    self.CheckGeoTransform((100000, 10, 0, 5000000, 0, -10))

  def testNitf53UtmMgrs(self):
    with gcore_util.TestTemporaryDirectory(prefix='nitf45') as tmpdir:
      filepath = os.path.join(tmpdir, 'nitf53.ntf')
      src = self.driver.Create(filepath, 2, 2, options=['ICORDS=N'])
      self.assertTrue(src)
      src = None

      with open(filepath, 'r+b') as nitf_file:
        nitf_file.seek(775)
        nitf_file.write(b'U')
        nitf_file.write(b'31UBQ1000040000')
        nitf_file.write(b'31UBQ2000040000')
        nitf_file.write(b'31UBQ2000030000')
        nitf_file.write(b'31UBQ1000030000')

      self.CheckOpen(filepath)
      self.CheckProjection(
          'PROJCS["UTM Zone 31, Northern Hemisphere",'
          '    GEOGCS["WGS 84",'
          '        DATUM["WGS_1984",'
          '            SPHEROID["WGS 84",6378137,298.257223563,'
          '                AUTHORITY["EPSG","7030"]],'
          '            TOWGS84[0,0,0,0,0,0,0],'
          '            AUTHORITY["EPSG","6326"]],'
          '        PRIMEM["Greenwich",0,'
          '            AUTHORITY["EPSG","8901"]],'
          '        UNIT["degree",0.0174532925199433,'
          '            AUTHORITY["EPSG","9108"]],'
          '        AUTHORITY["EPSG","4326"]],'
          '    PROJECTION["Transverse_Mercator"],'
          '    PARAMETER["latitude_of_origin",0],'
          '    PARAMETER["central_meridian",3],'
          '    PARAMETER["scale_factor",0.9996],'
          '    PARAMETER["false_easting",500000],'
          '    PARAMETER["false_northing",0],'
          '    UNIT["Meter",1]]'
          )
      self.CheckGeoTransform((205000, 10000, 0, 5445000, 0, -10000))

  def testNitf54TreRpc00b(self):
    filepath = '/vsimem/test_nitf54.ntf'
    # Fake non-conformant RPC00B TRE enough to test GDAL.
    rpc00b = '1' + '0' * 1040
    options = ['TRE=RPC00B=' + rpc00b]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None
    self.dst = self.CheckOpen(filepath)
    self.assertIn('HEIGHT_OFF', self.src.GetMetadata('RPC'))

  def testNitf55TreIchipb(self):
    filepath = '/vsimem/test_nitf55.ntf'
    # Fake non-conformant RPC00B TRE enough to test GDAL.
    ichipb = '0' * 224
    options = ['TRE=ICHIPB=' + ichipb]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None
    self.dst = self.CheckOpen(filepath)
    self.assertIn('ICHIP_SCALE_FACTOR', self.src.GetMetadata())

  def testNitf56TreUse00a(self):
    filepath = '/vsimem/test_nitf56.ntf'
    # Fake non-conformant TRE enough to test GDAL.
    use00a = '0' * 107
    options = ['TRE=USE00A=' + use00a]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None
    self.dst = self.CheckOpen(filepath)
    self.assertIn('NITF_USE00A_ANGLE_TO_NORTH', self.src.GetMetadata())

  def testNitf57TreGeolob(self):
    filepath = '/vsimem/test_nitf57.ntf'
    geolob = '000000360000000360-180.000000000090.000000000000'
    options = ['TRE=GEOLOB=' + geolob]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None
    self.dst = self.CheckOpen(filepath)
    self.CheckGeoTransform((-180, 1, 0, 90, 0, -1))

  def testNitf58TreStdidc(self):
    filepath = '/vsimem/test_nitf58.ntf'
    stdidc = '0' * 89
    options = ['TRE=STDIDC=' + stdidc]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None
    self.dst = self.CheckOpen(filepath)
    self.assertIn('NITF_STDIDC_ACQUISITION_DATE', self.src.GetMetadata())

  def testNitf59(self):
    filepath_orig = gdrivers_util.GetTestFilePath('nitf59')
    with gcore_util.TestTemporaryDirectory(prefix='nitf59') as tmpdir:
      for ext in ('.nfw', '.hdr'):
        shutil.copy(filepath_orig + ext, os.path.join(tmpdir, 'nitf59' + ext))
      filepath = os.path.join(tmpdir, 'nitf59.ntf')
      src = self.driver.Create(filepath, 1, 1, options=['ICORDS=N'])
      self.assertTrue(src)
      src = None

      self.CheckOpen(filepath)
      self.CheckProjection(
          'PROJCS["UTM Zone 31, Northern Hemisphere",'
          '    GEOGCS["WGS 84",'
          '        DATUM["WGS_1984",'
          '            SPHEROID["WGS 84",6378137,298.257223563,'
          '                AUTHORITY["EPSG","7030"]],'
          '            TOWGS84[0,0,0,0,0,0,0],'
          '            AUTHORITY["EPSG","6326"]],'
          '        PRIMEM["Greenwich",0,'
          '            AUTHORITY["EPSG","8901"]],'
          '        UNIT["degree",0.0174532925199433,'
          '            AUTHORITY["EPSG","9108"]],'
          '        AUTHORITY["EPSG","4326"]],'
          '    PROJECTION["Transverse_Mercator"],'
          '    PARAMETER["latitude_of_origin",0],'
          '    PARAMETER["central_meridian",3],'
          '    PARAMETER["scale_factor",0.9996],'
          '    PARAMETER["false_easting",500000],'
          '    PARAMETER["false_northing",0],'
          '    UNIT["Meter",1]]'
          )
      self.CheckGeoTransform((149999.5, 1, 0, 4500000.5, 0, -1))

  # TODO(schwehr): testNitf60CadrgPolarTileGeoRef(self)

  def testNitf61TreDe(self):
    # pylint:disable=line-too-long
    # From  http://www.gwg.nga.mil/ntb/baseline/software/testfile/rsm/SampleFiles/FrameSet1/NITF_Files/i_6130a.zip
    filepath = gdrivers_util.GetTestFilePath('i_6130a_truncated.ntf')
    self.CheckOpen(filepath)
    xml_tre = self.src.GetMetadata('xml:TRE')[0]
    self.assertIn('RSMDCA', xml_tre)
    self.assertIn('RSMECA', xml_tre)
    self.assertIn('RSMPCA', xml_tre)
    self.assertIn('RSMIDA', xml_tre)
    self.assertIn('<tre name="RSMDCA"', xml_tre)

  def testNitf62Comments(self):
    filepath = '/vsimem/test_nitf58.ntf'
    comments = '0123456789' * 9 + '012345678ZA'
    options = ['ICOM=' + comments]
    src = self.driver.Create(filepath, 1, 1, options=options)
    self.assertTrue(src)
    src = None
    self.CheckOpen(filepath)
    result_comments = self.src.GetMetadata()['NITF_IMAGE_COMMENTS']
    self.assertIn(comments, result_comments)

  def testNitf63LineWhenNumColsLessThanBlockWidth(self):
    # http://trac.osgeo.org/gdal/ticket/3551
    with gcore_util.TestTemporaryDirectory(prefix='nitf63') as tmpdir:
      filepath = os.path.join(tmpdir, 'nitf63.ntf')
      src = self.driver.Create(filepath, 50, 25, 3, gdal.GDT_Int16,
                               options=['BLOCKXSIZE=256'])
      src = None
      with open(filepath, 'r+b') as nitf_file:
        nitf_file.seek(820)
        nitf_file.write(b'P')

      src = gdal.Open(filepath, gdal.GA_Update)
      self.assertEqual(src.GetMetadata()['NITF_IMODE'], 'P')
      src.GetRasterBand(1).Fill(0)
      src.GetRasterBand(2).Fill(127)
      src.GetRasterBand(3).Fill(255)
      src = None

      self.CheckOpen(filepath)
      for band_num, checksum in zip((1, 2, 3), (0, 14186, 15301)):
        self.CheckBand(band_num, checksum, gdal.GDT_Int16)

  def testNitf64SdeTre(self):
    tiff_drv = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    src = tiff_drv.Create('/vsimem/nitf.tif', 256, 256, 1)
    src.SetGeoTransform([2.123456789, 0.123456789, 0, 49.123456789, 0,
                         -0.123456789])
    srs = osr.SpatialReference()
    srs.SetWellKnownGeogCS('WGS84')
    src.SetProjection(srs.ExportToWkt())
    self.src = src
    self.CheckCreateCopy(options=['ICORDS=D'])
    self.CheckCreateCopy(options=['ICORDS=G'])
    self.CheckCreateCopy(options=['SDE_TRE=YES'])

  # TODO(schwehr): Tests 65 to 70.
  # TODO(schwehr): Online tests, but load the data locally.

if __name__ == '__main__':
  unittest.main()
