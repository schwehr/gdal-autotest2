# Copyright 2025 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2022, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT

"""Test the JPEG XL/JPEGXL/JXL raster driver in GDAL.

Format is described here:

https://gdal.org/drivers/raster/jpegxl.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpegxl.py
"""
import base64
import glob
import os
import pathlib
import struct

from osgeo import gdal

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.JPEGXL_DRIVER
EXT = '.jxl'


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class JpegxlInfoTest(gdrivers_util.DriverTestCase):

  def setUp(self):  # pytype: disable=signature-mismatch
    super().setUp(DRIVER, EXT)

  def testInfo(self):
    with gdal.quiet_errors():
      globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
      for jsonfile in sorted(glob.glob(globpath)):
        filepath = os.path.splitext(jsonfile)[0]
        with self.subTest(input=os.path.basename(filepath)):
          self.CheckOpen(filepath)
          self.CheckInfo()


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class JpegxlTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pytype: disable=signature-mismatch
    super().setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  # Skip test_jpegxl_read. byte.jxl in the info tests.

  def test_jpegxl_byte(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(vsimem=True)

  def test_jpegxl_uint16(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/uint16.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(vsimem=True)

  def test_jpegxl_float32(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/float32.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(vsimem=True)

  def test_jpegxl_grey_alpha(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/stefan_full_greyalpha.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(vsimem=True)

  def test_jpegxl_rgb(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(vsimem=True)

  def test_jpegxl_rgba(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(vsimem=True)

  @parameterized.parameters(['YES', 'NO', None])
  def test_jpegxl_rgba_lossless_param(self, lossless):
    src_filename = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    src_ds = gdal.Open(src_filename)
    filename = '/vsimem/lossless_{lossless}.jxl'
    options = []
    if lossless:
      options += ['LOSSLESS=' + lossless]
    self.driver.CreateCopy(filename, src_ds, options=options)

    ds = gdal.Open(filename)
    reversibility = ds.GetMetadataItem(
        'COMPRESSION_REVERSIBILITY', 'IMAGE_STRUCTURE'
    )
    if lossless == 'NO':
      self.assertEqual(reversibility, 'LOSSY')
    else:
      self.assertEqual(reversibility, 'LOSSLESS (possibly)')

    ds = None  # pylint: disable=unused-variable
    self.driver.Delete(filename)

  def test_jpegxl_rgba_lossless_no_but_lossless_copy_yes(self):
    src_filename = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    src_ds = gdal.Open(src_filename)
    filename = '/vsimem/lossless_no_but_lossless_copy_yes.jxl'

    with gdal.quiet_errors():
      options = ['LOSSLESS=NO', 'LOSSLESS_COPY=YES']
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=options)
      )
    self.assertIsNone(gdal.VSIStatL(filename))

  def test_jpegxl_rgba_distance(self):
    src_filename = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    src_ds = gdal.Open(src_filename)
    src_ds_checksum = src_ds.GetRasterBand(1).Checksum()

    filename = '/vsimem/test_jpegxl_rgba_distance.jxl'

    self.driver.CreateCopy(filename, src_ds, options=['DISTANCE=2'])
    with gdal.Open(filename) as ds:
      self.assertEqual(
          ds.GetMetadataItem('COMPRESSION_REVERSIBILITY', 'IMAGE_STRUCTURE'),
          'LOSSY',
      )
      ds_checksum = ds.GetRasterBand(1).Checksum()
      self.assertNotIn(ds_checksum, [0, src_ds_checksum])

    self.driver.Delete(filename)

  @parameterized.parameters([(100, 0), (10, 15.267)])
  def test_jpegxl_rgba_quality(self, quality, equivalent_distance):
    src_filename = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    src_ds = gdal.Open(src_filename)
    filename = '/vsimem/test_jpegxl_rgba_quality.jxl'

    options = [f'QUALITY={quality}']
    with self.driver.CreateCopy(filename, src_ds, options=options) as ds:
      self.assertEqual(
          ds.GetMetadataItem('COMPRESSION_REVERSIBILITY', 'IMAGE_STRUCTURE'),
          'LOSSY',
      )
      cs = ds.GetRasterBand(1).Checksum()
      self.assertNotIn(cs, [0, src_ds.GetRasterBand(1).Checksum()])

    with gdal.quiet_errors():
      options = [f'DISTANCE={equivalent_distance}']
      with self.driver.CreateCopy(filename, src_ds, options=options) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), cs)

    self.driver.Delete(filename)

  def test_jpegxl_xmp(self):
    filename = '/vsimem/test_jpegxl_xmp.jxl'
    src_ds = gdal.Open(self.getTestFilePath('byte_with_xmp.tif'))

    with self.driver.CreateCopy(filename, src_ds) as ds:
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))

      # TODO(schwehr): Why the extra empty string?
      expect = set(['', 'DERIVED_SUBDATASETS', 'xml:XMP', 'IMAGE_STRUCTURE'])
      self.assertEqual(set(ds.GetMetadataDomainList()), expect)
      self.assertTrue(ds.GetMetadata('xml:XMP')[0].startswith('<?xpacket'))

  def test_jpegxl_exif(self):
    filename = '/vsimem/test_jpegxl_exif.jxl'

    src_ds = gdal.Open(self.getTestFilePath('exif_and_gps.tif'))
    self.driver.CreateCopy(filename, src_ds)
    # Do not unlink the aux file. It does not exist in google3.
    self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))

    with gdal.Open(filename) as ds:
      expect = set(['DERIVED_SUBDATASETS', 'IMAGE_STRUCTURE', 'EXIF'])
      self.assertEqual(set(ds.GetMetadataDomainList()), expect)
      self.assertEqual(src_ds.GetMetadata('EXIF'), ds.GetMetadata('EXIF'))

    self.driver.Delete(filename)

  # Skip test_jpegxl_read_huge_xmp_compressed_box. Info test with
  # huge_xmp_compressed_box.jxl

  def test_jpegxl_uint8_7_bits(self):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif'))

    options = '-of MEM -scale 0 255 0 127'
    rescaled = gdal.Translate('', src_ds, options=options)

    filename = '/vsimem/test_jpegxl_uint8_7_bits.jxl'
    with self.driver.CreateCopy(filename, rescaled, options=['NBITS=7']) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), rescaled.GetRasterBand(1).Checksum())
      self.assertEqual(band.GetMetadataItem('NBITS', 'IMAGE_STRUCTURE'), '7')

    self.driver.Delete(filename)

  def test_jpegxl_uint16_12_bits(self):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/uint16.tif'))
    filename = '/vsimem/test_jpegxl_uint16_12_bits.jxl'
    with self.driver.CreateCopy(filename, src_ds, options=['NBITS=12']) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), src_ds.GetRasterBand(1).Checksum())
      self.assertEqual(band.GetMetadataItem('NBITS', 'IMAGE_STRUCTURE'), '12')

    self.driver.Delete(filename)

  def test_jpegxl_rasterio(self):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif'))
    filename = '/vsimem/test_jpegxl_rasterio.jxl'

    with self.driver.CreateCopy(filename, src_ds) as ds:
      # Run twice to check that internal deferred decoding works properly.
      for i in range(2):
        del i  # Unused.
        result = ds.ReadRaster(
            buf_pixel_space=3,
            buf_line_space=3 * src_ds.RasterXSize,
            buf_band_space=1,
        )
        expect = src_ds.ReadRaster(
            buf_pixel_space=3,
            buf_line_space=3 * src_ds.RasterXSize,
            buf_band_space=1,
        )
        self.assertEqual(result, expect)

      # Do not use block cache.
      result = ds.ReadRaster(
          buf_type=gdal.GDT_UInt16,
          buf_pixel_space=2 * 3,
          buf_line_space=2 * 3 * src_ds.RasterXSize,
          buf_band_space=2,
      )
      expect = src_ds.ReadRaster(
          buf_type=gdal.GDT_UInt16,
          buf_pixel_space=2 * 3,
          buf_line_space=2 * 3 * src_ds.RasterXSize,
          buf_band_space=2,
      )
      self.assertEqual(result, expect)

      result = ds.ReadRaster(
          band_list=[1, 2],
          buf_type=gdal.GDT_UInt16,
          buf_pixel_space=2 * 2,
          buf_line_space=2 * 2 * src_ds.RasterXSize,
          buf_band_space=2,
      )
      expect = src_ds.ReadRaster(
          band_list=[1, 2],
          buf_type=gdal.GDT_UInt16,
          buf_pixel_space=2 * 2,
          buf_line_space=2 * 2 * src_ds.RasterXSize,
          buf_band_space=2,
      )
      self.assertEqual(result, expect)

      # Band interleaved spacing.
      self.assertEqual(
          ds.ReadRaster(buf_type=gdal.GDT_UInt16),
          src_ds.ReadRaster(buf_type=gdal.GDT_UInt16),
      )
      self.assertEqual(
          ds.ReadRaster(band_list=[2, 1]), src_ds.ReadRaster(band_list=[2, 1])
      )

      # Regular code path.
      self.assertEqual(
          ds.ReadRaster(0, 0, 10, 10), src_ds.ReadRaster(0, 0, 10, 10)
      )

    self.driver.Delete(filename)

  def test_jpegxl_icc_profile(self):
    filename = '/vsimem/test_jpegxl_icc_profile.jxl'
    data = pathlib.Path(self.getTestFilePath('sRGB.icc')).read_bytes()
    icc = base64.b64encode(data).decode('ascii')
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with mem_driver.Create('', 1, 1, 3) as src_ds:
      src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand)
      src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_GreenBand)
      src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_BlueBand)
      src_ds.SetMetadataItem('SOURCE_ICC_PROFILE', icc, 'COLOR_PROFILE')

      with self.driver.CreateCopy(filename, src_ds) as ds:
        result = ds.GetMetadataItem('SOURCE_ICC_PROFILE', 'COLOR_PROFILE')
        self.assertEqual(result, icc)

    self.driver.Delete(filename)

  def test_jpegxl_lossless_copy_of_jpeg(self):
    src_filename = gdrivers_util.GetTestFilePath('jpeg/albania.jpg')
    src_ds = gdal.Open(src_filename)
    filename = '/vsimem/test_jpegxl_lossless_copy_of_jpeg.jxl'
    with self.driver.CreateCopy(filename, src_ds) as ds:
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
      # TODO(schwehr): Why the extra empty string?
      expect = set(['', 'DERIVED_SUBDATASETS', 'EXIF', 'IMAGE_STRUCTURE'])
      self.assertEqual(set(ds.GetMetadataDomainList()), expect)

      result = ds.GetMetadataItem(
          'COMPRESSION_REVERSIBILITY', 'IMAGE_STRUCTURE'
      )
      self.assertEqual(result, 'LOSSY')

      result = ds.GetMetadataItem('ORIGINAL_COMPRESSION', 'IMAGE_STRUCTURE')
      self.assertEqual(result, 'JPEG')

    self.driver.Delete(filename)

    # Test failure in JxlEncoderAddJPEGFrame() by adding a truncated JPEG file.
    # TODO(schwehr): Port this part of the test.

  def test_jpegxl_lossless_copy_of_jpeg_disabled(self):
    src_filename = gdrivers_util.GetTestFilePath('jpeg/albania.jpg')
    src_ds = gdal.Open(src_filename)
    filename = '/vsimem/test_jpegxl_lossless_copy_of_jpeg_disabled.jxl'
    options = ['LOSSLESS_COPY=NO']
    with self.driver.CreateCopy(filename, src_ds, options=options) as ds:
      result = ds.GetMetadataItem('ORIGINAL_COMPRESSION', 'IMAGE_STRUCTURE')
      self.assertNotEqual(result, 'JPEG')

    self.driver.Delete(filename)

  def test_jpegxl_lossless_copy_of_jpeg_with_mask_band(self):
    self.assertIsNotNone(
        self.driver.GetMetadataItem('JXL_ENCODER_SUPPORT_EXTRA_CHANNELS')
    )
    self.assertIn(
        'COMPRESS_BOX', self.driver.GetMetadataItem('DMD_CREATIONOPTIONLIST')
    )

    jpeg_driver = gdal.GetDriverByName(gdrivers_util.JPEG_DRIVER)

    filename = '/vsimem/test_jpegxl_lossless_copy_of_jpeg_with_mask_band.jxl'
    filename_jpg = (
        '/vsimem/test_jpegxl_lossless_copy_of_jpeg_with_mask_band.jpg'
    )

    src_filename = gdrivers_util.GetTestFilePath('jpeg/masked.jpg')
    src_ds = gdal.Open(src_filename)
    options = ['LOSSLESS_COPY=YES']
    # jxl/encode.cc:1117: Color encoding is already set
    with self.driver.CreateCopy(filename, src_ds, options=options) as ds:
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
      self.assertEqual(ds.RasterCount, 4)

      result = ds.GetRasterBand(4).Checksum()
      expect = src_ds.GetRasterBand(1).GetMaskBand().Checksum()
      self.assertEqual(result, expect)

      result = ds.GetMetadataItem(
          'COMPRESSION_REVERSIBILITY', 'IMAGE_STRUCTURE'
      )
      self.assertEqual(result, 'LOSSY')

      result = ds.GetMetadataItem('ORIGINAL_COMPRESSION', 'IMAGE_STRUCTURE')
      self.assertEqual(result, 'JPEG')

      with jpeg_driver.CreateCopy(filename_jpg, src_ds) as ds:
        self.assertEqual(ds.RasterCount, 3)

        self.assertEqual(
            ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
        )
        self.assertEqual(
            ds.GetRasterBand(2).Checksum(), src_ds.GetRasterBand(2).Checksum()
        )
        self.assertEqual(
            ds.GetRasterBand(3).Checksum(), src_ds.GetRasterBand(3).Checksum()
        )
        self.assertEqual(
            ds.GetRasterBand(1).GetMaskBand().Checksum(),
            src_ds.GetRasterBand(1).GetMaskBand().Checksum(),
        )

    self.driver.Delete(filename)
    jpeg_driver.Delete(filename_jpg)

  def test_jpegxl_lossless_copy_of_jpeg_xmp(self):
    jpeg_driver = gdal.GetDriverByName(gdrivers_util.JPEG_DRIVER)

    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('jpeg/byte_with_xmp.jpg'))

    filename = '/vsimem/test_jpegxl_lossless_copy_of_jpeg_xmp.jxl'
    filename_jpg = '/vsimem/test_jpegxl_lossless_copy_of_jpeg_xmp.jpg'

    with self.driver.CreateCopy(filename, src_ds) as ds:
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
      with jpeg_driver.CreateCopy(filename_jpg, ds) as ds_jpeg:
        self.assertIsNotNone(ds_jpeg)
        self.assertEqual(
            ds_jpeg.GetMetadata('xml:XMP'), src_ds.GetMetadata('xml:XMP')
        )

    self.driver.Delete(filename)
    jpeg_driver.Delete(filename_jpg)

  def test_jpegxl_read_extra_channels(self):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif'))
    with gdal.Open(self.getTestFilePath('threeband_non_rgb.jxl')) as ds:
      count = src_ds.RasterCount
      self.assertEqual(ds.RasterCount, count)
      result = [ds.GetRasterBand(i + 1).Checksum() for i in range(count)]
      expect = [src_ds.GetRasterBand(i + 1).Checksum() for i in range(count)]
      self.assertEqual(result, expect)

      self.assertEqual(ds.ReadRaster(), src_ds.ReadRaster())

  def test_jpegxl_write_extra_channels(self):
    self.assertIsNotNone(
        self.driver.GetMetadataItem('JXL_ENCODER_SUPPORT_EXTRA_CHANNELS')
    )

    src_filename = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    src_ds = gdal.Open(src_filename)

    x_size = src_ds.RasterXSize
    y_size = src_ds.RasterYSize
    count = src_ds.RasterCount

    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    mem_ds = mem_driver.Create('', x_size, y_size, count)
    mem_ds.WriteRaster(0, 0, x_size, y_size, src_ds.ReadRaster())
    mem_ds.GetRasterBand(3).SetDescription('third channel')

    filename = '/vsimem/test_jpegxl_write_extra_channels.jxl'
    with self.driver.CreateCopy(filename, mem_ds) as ds:
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))

      result = [ds.GetRasterBand(i + 1).Checksum() for i in range(count)]
      expect = [mem_ds.GetRasterBand(i + 1).Checksum() for i in range(count)]
      self.assertEqual(result, expect)

      self.assertEqual(ds.ReadRaster(), mem_ds.ReadRaster())
      self.assertEmpty(ds.GetRasterBand(1).GetDescription())
      # 'Band 2' encoded in .jxl file, but hidden when reading back
      self.assertEmpty(ds.GetRasterBand(2).GetDescription())
      self.assertEqual(ds.GetRasterBand(3).GetDescription(), 'third channel')

      self.driver.Delete(filename)

  def test_jpegxl_read_five_bands(self):
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    checksums = [3741, 5281, 6003, 5095, 4318]
    with gdal.Open(self.getTestFilePath('five_bands.jxl')) as ds:
      with mem_driver.CreateCopy('', ds) as mem_ds:
        self.assertEqual(ds.ReadRaster(), mem_ds.ReadRaster())
        result = [mem_ds.GetRasterBand(i + 1).Checksum() for i in range(5)]
        self.assertEqual(result, checksums)
        self.assertEqual(
            ds.ReadRaster(band_list=[1]), mem_ds.ReadRaster(band_list=[1])
        )

        mem_data = mem_ds.ReadRaster(
            buf_pixel_space=ds.RasterCount,
            buf_line_space=ds.RasterCount * ds.RasterXSize,
            buf_band_space=1,
        )

      ds_data = ds.ReadRaster(
          buf_pixel_space=ds.RasterCount,
          buf_line_space=ds.RasterCount * ds.RasterXSize,
          buf_band_space=1,
      )

      self.assertEqual(ds_data, mem_data)

  def test_jpegxl_write_five_bands(self):
    self.assertIsNotNone(
        self.driver.GetMetadataItem('JXL_ENCODER_SUPPORT_EXTRA_CHANNELS')
    )
    filename = '/vsimem/test_jpegxl_write_five_bands.jxl'
    checksums = [3741, 5281, 6003, 5095, 4318]

    with gdal.Open(self.getTestFilePath('five_bands.jxl')) as src_ds:
      with self.driver.CreateCopy(filename, src_ds) as ds:
        result = [ds.GetRasterBand(i + 1).Checksum() for i in range(5)]
        self.assertEqual(result, checksums)

    self.driver.Delete(filename)

  def test_jpegxl_write_five_bands_lossy(self):
    filename = '/vsimem/test_jpegxl_write_five_bands_lossy.jxl'

    with gdal.Open(self.getTestFilePath('five_bands.jxl')) as src_ds:
      options = '-of JPEGXL -co DISTANCE=3 -ot Byte'
      with gdal.Translate(filename, src_ds, options=options) as ds:
        for i in range(5):
          min_val, max_val = ds.GetRasterBand(i + 1).ComputeRasterMinMax()
          min_expect = 10.0 * (i + 1)
          max_expect = 10.0 * (i + 1)
          self.assertAlmostEqual(min_val, min_expect, delta=1)
          self.assertAlmostEqual(max_val, max_expect, delta=1)

    self.driver.Delete(filename)

  def test_jpegxl_createcopy_errors(self):
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    filename = '/vsimem/test_jpegxl_createcopy_errors.jxl'

    # band count = 0
    src_ds = mem_driver.Create('', 1, 1, 0)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      self.assertIsNone(self.driver.CreateCopy(filename, src_ds))
      # "'Invalid source dataset' has length of 22."
      message = 'Invalid source dataset'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1, 1, gdal.GDT_Int16)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      self.assertIsNone(self.driver.CreateCopy(filename, src_ds))
      message = 'Unsupported data type'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      does_not_exist = '/does/not/exist.jxl'
      self.assertIsNone(self.driver.CreateCopy(does_not_exist, src_ds))
      message = f'Cannot create {does_not_exist}: No such file or directory'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      options = ['LOSSLESS=YES', 'DISTANCE=1']
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=options)
      )
      message = 'DISTANCE and LOSSLESS=YES are mutually exclusive'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      options = ['LOSSLESS=YES', 'ALPHA_DISTANCE=1']
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=options)
      )
      message = 'ALPHA_DISTANCE and LOSSLESS=YES are mutually exclusive'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      options = ['LOSSLESS=YES', 'QUALITY=90']
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=options)
      )
      message = 'QUALITY and LOSSLESS=YES are mutually exclusive'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      options = ['DISTANCE=1', 'QUALITY=90']
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=options)
      )
      message = 'QUALITY and DISTANCE are mutually exclusive'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=['DISTANCE=-1'])
      )
      # jxl/encode.cc: Distance has to be in [0.0..25.0]
      #   (corresponding to quality in [0.0..100.0])
      message = 'JxlEncoderSetFrameDistance() failed'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    src_ds = mem_driver.Create('', 1, 1)
    with gdal.quiet_errors():
      gdal.ErrorReset()
      self.assertIsNone(
          self.driver.CreateCopy(filename, src_ds, options=['EFFORT=-1'])
      )
      # jxl/encode.cc: "Encode effort has to be in [1..10]"
      message = 'JxlEncoderFrameSettingsSetOption() failed'
      self.assertEqual(gdal.GetLastErrorMsg(), message)

    gdal.ErrorReset()

  def test_jpegxl_band_combinations(self):
    filename = '/vsimem/test_jpegxl_band_combinations.jxl'
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    src_ds = mem_driver.Create('', 64, 64, 6)
    for b in range(6):
      band = src_ds.GetRasterBand(b + 1)
      band.Fill(b + 1)
      band.FlushCache()
      self.assertNotEqual(band.Checksum(), 0)

    cilists = [
        [gdal.GCI_RedBand],
        [gdal.GCI_RedBand, gdal.GCI_Undefined],
        [gdal.GCI_RedBand, gdal.GCI_AlphaBand],
        [gdal.GCI_Undefined, gdal.GCI_AlphaBand],
        [gdal.GCI_RedBand, gdal.GCI_GreenBand, gdal.GCI_BlueBand],
        [
            gdal.GCI_RedBand,
            gdal.GCI_GreenBand,
            gdal.GCI_BlueBand,
            gdal.GCI_AlphaBand,
        ],
        [
            gdal.GCI_RedBand,
            gdal.GCI_GreenBand,
            gdal.GCI_BlueBand,
            gdal.GCI_AlphaBand,
            gdal.GCI_Undefined,
        ],
        [
            gdal.GCI_RedBand,
            gdal.GCI_GreenBand,
            gdal.GCI_BlueBand,
            gdal.GCI_Undefined,
            gdal.GCI_Undefined,
        ],
        [
            gdal.GCI_RedBand,
            gdal.GCI_GreenBand,
            gdal.GCI_BlueBand,
            gdal.GCI_Undefined,
            gdal.GCI_AlphaBand,
        ],
        [
            gdal.GCI_RedBand,
            gdal.GCI_GreenBand,
            gdal.GCI_AlphaBand,
            gdal.GCI_Undefined,
            gdal.GCI_BlueBand,
        ],
    ]

    types = [
        gdal.GDT_Byte,
        gdal.GDT_UInt16,
    ]

    for dtype in types:
      for cilist in cilists:
        bandlist = [idx + 1 for idx in range(len(cilist))]
        vrtds = gdal.Translate(
            '', src_ds, format='vrt', bandList=bandlist, outputType=dtype
        )
        for idx, ci in enumerate(cilist):
          vrtds.GetRasterBand(idx + 1).SetColorInterpretation(ci)

        with gdal.Translate(filename, vrtds) as ds:
          self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
          for idx in range(len(cilist)):
            result = ds.GetRasterBand(idx + 1).Checksum()
            expect = src_ds.GetRasterBand(idx + 1).Checksum()
            self.assertEqual(result, expect)

          if (
              vrtds.RasterCount >= 3
              and vrtds.GetRasterBand(1).GetColorInterpretation()
              == gdal.GCI_RedBand
              and vrtds.GetRasterBand(2).GetColorInterpretation()
              == gdal.GCI_GreenBand
              and vrtds.GetRasterBand(3).GetColorInterpretation()
              == gdal.GCI_BlueBand
          ):
            self.assertEqual(
                ds.GetRasterBand(1).GetColorInterpretation(), gdal.GCI_RedBand
            )
            self.assertEqual(
                ds.GetRasterBand(2).GetColorInterpretation(), gdal.GCI_GreenBand
            )
            self.assertEqual(
                ds.GetRasterBand(3).GetColorInterpretation(), gdal.GCI_BlueBand
            )

          for idx in range(len(cilist)):
            vrtds_interp = vrtds.GetRasterBand(idx + 1).GetColorInterpretation()
            if vrtds_interp == gdal.GCI_AlphaBand:
              color_interp = ds.GetRasterBand(idx + 1).GetColorInterpretation()
              self.assertEqual(color_interp, gdal.GCI_AlphaBand)

  @parameterized.parameters([i + 1 for i in range(8)])
  def test_jpegxl_apply_orientation(self, orientation):
    open_options = self.driver.GetMetadataItem('DMD_OPENOPTIONLIST')
    self.assertIn('APPLY_ORIENTATION', open_options)

    filename = self.getTestFilePath(f'exif_orientation/F{orientation}.jxl')
    open_options = ['APPLY_ORIENTATION=YES']
    with gdal.OpenEx(filename, open_options=open_options) as ds:
      self.assertEqual(ds.RasterXSize, 3)
      self.assertEqual(ds.RasterYSize, 5)

      vals = struct.unpack('B' * 3 * 5, ds.ReadRaster())
      vals = [1 if v >= 128 else 0 for v in vals]
      self.assertEqual(vals, [1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0])

      if orientation != 1:
        self.assertIsNone(ds.GetMetadataItem('EXIF_Orientation', 'EXIF'))
        result = ds.GetMetadataItem('original_EXIF_Orientation', 'EXIF')
        self.assertEqual(result, str(orientation))

  def test_jpegxl_alpha_distance_zero(self):
    src_filename = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    src_ds = gdal.Open(src_filename)
    filename = '/vsimem/test_jpegxl_alpha_distance_zero.jxl'
    options = ['LOSSLESS=NO', 'ALPHA_DISTANCE=0']
    with self.driver.CreateCopy(filename, src_ds, options=options) as ds:
      self.assertNotEqual(
          ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
      )
      self.assertEqual(
          ds.GetRasterBand(4).Checksum(), src_ds.GetRasterBand(4).Checksum()
      )

    self.driver.Delete(filename)

  # Skip test_jpegxl_identify_raw_codestream - gdalmanage not in google3.
  # Skip test_jpegxl_read_float16 - info test with float16.jxl.


if __name__ == '__main__':
  googletest.main()
