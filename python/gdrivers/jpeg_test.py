# Copyright 2016 Google Inc. All Rights Reserved.
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

https://gdal.org/drivers/raster/jpeg.html

Rewrite of jpeg.py:

https://github.com/OSGeo/gdal/blob/ea75dfb98cb6a6fe27d826f840178e8e6b6fe626/autotest/gdrivers/jpeg.py
"""

import glob
import os
import shutil
import struct
from unittest import mock

from osgeo import gdal

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.JPEG_DRIVER
EXT = '.jpg'


def _make_marker_segment(marker_byte: int, payload: bytes) -> bytes:
  """Builds a JPEG APP/COM marker segment from its marker byte and payload."""
  return (
      b'\xff'
      + bytes([marker_byte])
      + struct.pack('>H', len(payload) + 2)
      + payload
  )


def _inject_after_soi(jpeg_bytes: bytes, segment: bytes) -> bytes:
  """Injects a JPEG marker segment immediately after the SOI (0xFFD8) marker."""
  if not jpeg_bytes.startswith(b'\xff\xd8'):
    raise ValueError('Input bytes do not start with JPEG SOI marker')
  return jpeg_bytes[:2] + segment + jpeg_bytes[2:]


def _create_base_jpeg_bytes(
    width: int,
    height: int,
    bands: int,
    exif_make: str | None = None,
) -> bytes:
  """Creates a minimal JPEG byte stream in /vsimem/ with optional EXIF_Make."""
  mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
  jpeg_drv = gdal.GetDriverByName(DRIVER)
  src_ds = mem_drv.Create('', width, height, bands, gdal.GDT_Byte)
  for band_idx in range(1, bands + 1):
    src_ds.GetRasterBand(band_idx).Fill(band_idx * 40)
  if exif_make is not None:
    src_ds.SetMetadataItem('EXIF_Make', exif_make)
  vsi_path = '/vsimem/_synthetic_base.jpg'
  with gcore_util.GdalUnlinkWhenDone(vsi_path):
    dst_ds = jpeg_drv.CreateCopy(vsi_path, src_ds)
    del dst_ds
    src_ds = None
    stat_buf = gdal.VSIStatL(vsi_path)
    return gcore_util.vsi_read_file_as_bytes(vsi_path, max_size=stat_buf.size)


def _build_flir_fff_buffer(
    raw_width: int,
    raw_height: int,
    raw_payload: bytes,
) -> bytes:
  """Constructs a minimal little-endian FLIR FFF buffer for testing."""
  header_and_dir_end = 64 + 3 * 32  # 160 bytes
  cam_offset = 288
  cam_len = 1126
  pal_offset = 1424
  pal_len = 118  # 112-byte header + 2 RGB palette entries
  raw_offset = 1552
  raw_rec_len = 32 + len(raw_payload)
  buf = bytearray(raw_offset + raw_rec_len)

  # FFF header: magic + creator, version=100 (20), dir offset=64 (24), count=3.
  buf[0:16] = b'FFF\x00TestCreator\x00'
  struct.pack_into('<III', buf, 20, 100, 64, 3)

  # Directory entries: CameraInfo (32), PaletteInfo (34), RawData (1).
  struct.pack_into('<H', buf, 64 + 0, 32)
  struct.pack_into('<II', buf, 64 + 12, cam_offset, cam_len)
  struct.pack_into('<H', buf, 96 + 0, 34)
  struct.pack_into('<II', buf, 96 + 12, pal_offset, pal_len)
  struct.pack_into('<H', buf, 128 + 0, 1)
  struct.pack_into('<II', buf, 128 + 12, raw_offset, raw_rec_len)
  assert header_and_dir_end < cam_offset

  # CameraInfo record (byte_order=2 -> little-endian).
  struct.pack_into('<H', buf, cam_offset, 2)
  # Emissivity, ObjectDistance (m), ReflectedApparentTemperature (K).
  struct.pack_into('<fff', buf, cam_offset + 32, 0.95, 2.0, 293.15)
  struct.pack_into('<f', buf, cam_offset + 60, 0.5)  # RelativeHumidity
  buf[cam_offset + 212 : cam_offset + 221] = b'FLIR_A65\x00'  # CameraModel
  struct.pack_into('<h', buf, cam_offset + 908, -60)  # Timezone offset (min)

  # PaletteInfo record: 2 colors, "Iron", RGB entries (10,20,30),(40,50,60).
  buf[pal_offset] = 2
  buf[pal_offset + 80 : pal_offset + 85] = b'Iron\x00'
  buf[pal_offset + 112 : pal_offset + 118] = bytes([10, 20, 30, 40, 50, 60])

  # RawData record: byte_order=2, width, height, and raw or PNG payload.
  struct.pack_into('<HHH', buf, raw_offset, 2, raw_width, raw_height)
  buf[raw_offset + 32 : raw_offset + 32 + len(raw_payload)] = raw_payload
  return bytes(buf)


def _build_flir_jpeg_bytes(
    raw_width: int,
    raw_height: int,
    raw_payload: bytes,
) -> bytes:
  """Builds a clean-room synthetic JPEG containing FLIR APP1 FFF metadata."""
  jpg_bytes = _create_base_jpeg_bytes(8, 8, 3, exif_make='FLIR')
  flir_buf = _build_flir_fff_buffer(raw_width, raw_height, raw_payload)
  app1_payload = b'FLIR\x00\x01\x00\x01' + flir_buf
  return _inject_after_soi(jpg_bytes, _make_marker_segment(0xE1, app1_payload))


def _build_dji_jpeg_bytes(
    width: int,
    height: int,
    raw_thermal_bytes: bytes,
) -> bytes:
  """Builds a clean-room synthetic DJI JPEG with EXIF, XMP, and APP3 thermal."""
  jpg_bytes = _create_base_jpeg_bytes(width, height, 3, exif_make='DJI')
  xmp_xml = (
      b'<x:xmpmeta xmlns:x="adobe:ns:meta/">'
      b'<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
      b'<rdf:Description xmlns:drone-dji="http://www.dji.com/drone-dji/1.0/" '
      b'drone-dji:Make="DJI"/>'
      b'</rdf:RDF></x:xmpmeta>'
  )
  xmp_payload = b'http://ns.adobe.com/xap/1.0/\x00' + xmp_xml
  jpg_bytes = _inject_after_soi(
      jpg_bytes, _make_marker_segment(0xE1, xmp_payload)
  )
  return _inject_after_soi(
      jpg_bytes, _make_marker_segment(0xE3, raw_thermal_bytes)
  )


class JpegTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testInfo(self):
    with gdal.quiet_errors():
      globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
      for jsonfile in sorted(glob.glob(globpath)):
        filepath = os.path.splitext(jsonfile)[0]
        with self.subTest(input=os.path.basename(filepath)):
          self.CheckOpen(filepath)
          self.CheckInfo()

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
        'EXIF_DateTimeDigitized': '    :  :     :  :',
        'EXIF_DateTimeOriginal': '    :  :     :  :',
        'EXIF_ExifVersion': '0210',
        'EXIF_FlashpixVersion': '0100',
        'EXIF_GPSLatitude': '(41) (1) (22.91)',
        'EXIF_GPSLatitudeRef': 'N',
        'EXIF_GPSLongitude': '(19) (55) (42.35)',
        'EXIF_GPSLongitudeRef': 'E',
        'EXIF_GPSVersionID': '0x02 0x00 0x00 0x00',
        'EXIF_Make': '',
        'EXIF_Model': '',
        'EXIF_PixelXDimension': '361',
        'EXIF_PixelYDimension': '260',
        'EXIF_ResolutionUnit': '2',
        'EXIF_XResolution': '(96)',
        'EXIF_YResolution': '(96)',
    }

    self.assertEqual(self.src.GetMetadata(), expected_metadata)  # pyrefly: ignore[missing-attribute]

    self.assertEqual(
        self.src.GetRasterBand(3).GetRasterColorInterpretation(),  # pyrefly: ignore[missing-attribute]
        gdal.GCI_BlueBand,
    )

    expected_structure = {
        'COMPRESSION': 'JPEG',
        'INTERLEAVE': 'PIXEL',
        'JPEG_QUALITY': '80',
        'SOURCE_COLOR_SPACE': 'YCbCr',
    }

    self.assertEqual(
        self.src.GetMetadata('IMAGE_STRUCTURE'),  # pyrefly: ignore[missing-attribute]
        expected_structure,
    )
    self.assertEqual(
        self.src.GetRasterBand(3).GetMetadataItem(  # pyrefly: ignore[missing-attribute]
            'COMPRESSION', 'IMAGE_STRUCTURE'
        ),
        'JPEG',
    )

    file_list = self.src.GetFileList()  # pyrefly: ignore[missing-attribute]
    self.assertLen(file_list, 1)
    self.assertEqual(os.path.basename(file_list[0]), 'albania.jpg')

  def test_jpeg_3(self):
    tmpdir = self.create_tempdir().full_path
    out_path = os.path.join(tmpdir, 'byte.jpg')
    wld_path = os.path.join(tmpdir, 'byte.wld')
    aux_path = out_path + '.aux.xml'

    with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src:
        options = ['PROGRESSIVE=YES', 'QUALITY=50', 'WORLDFILE=YES']
        ds = self.driver.CreateCopy(out_path, src, options=options)
        self.assertIsNotNone(ds)
        # IJG, MozJPEG.
        self.assertIn(ds.GetRasterBand(1).Checksum(), [4794, 4787])
        self.assertEqual(
            ds.GetRasterBand(1).GetRasterColorInterpretation(),
            gdal.GCI_GrayIndex,
        )
        expected_gt = (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0)
        self.assertIterAlmostEqual(
            ds.GetGeoTransform(), expected_gt, delta=1e-6
        )
        ds = None

      with gdal.Open(out_path) as ds:
        self.assertEqual(ds.GetMetadata(), {'AREA_OR_POINT': 'Area'})

      os.unlink(aux_path)
      self.assertTrue(os.path.exists(wld_path))

      with gdal.Open(out_path) as ds:
        self.assertIterAlmostEqual(
            ds.GetGeoTransform(), expected_gt, delta=1e-6
        )
        self.assertCountEqual(ds.GetFileList(), [out_path, wld_path])

      self.driver.Delete(out_path)
      self.assertFalse(os.path.exists(wld_path))

  def test_jpeg_4(self):
    with gdal.Open(self.getTestFilePath('masked.jpg')) as ds:
      self.assertEqual(
          ds.GetMetadataDomainList(),
          ['IMAGE_STRUCTURE', 'DERIVED_SUBDATASETS'],
      )
      refband = ds.GetRasterBand(1)
      self.assertEqual(refband.GetMaskFlags(), gdal.GMF_PER_DATASET)
      self.assertEqual(refband.GetMaskBand().Checksum(), 770)

  def test_jpeg_5(self):
    out_path = '/vsimem/test_jpeg_5_masked.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(self.getTestFilePath('masked.jpg')) as ds:
        with self.driver.CreateCopy(out_path, ds) as ds2:
          refband = ds2.GetRasterBand(1)
          self.assertEqual(refband.GetMaskFlags(), gdal.GMF_PER_DATASET)
          self.assertEqual(refband.GetMaskBand().Checksum(), 770)
      self.driver.Delete(out_path)

  def test_jpeg_6(self):
    gdal.ErrorReset()
    with gdal.Open(self.getTestFilePath('vophead.jpg')) as ds:
      self.assertFalse(
          gdal.GetLastErrorType() == gdal.CE_Warning
          and 'Ignoring EXIF' in gdal.GetLastErrorMsg()
      )
      with gdal.quiet_errors():
        md = ds.GetMetadata()
      self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
      self.assertIn('Ignoring EXIF', gdal.GetLastErrorMsg())
      self.assertEqual(md, {'EXIF_Software': 'IrfanView'})

  def test_jpeg_7(self):
    out_path = '/vsimem/test_jpeg_7.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src:
        options = ['PROGRESSIVE=YES', 'QUALITY=50']
        with self.driver.CreateCopy(out_path, src, options=options) as ds:
          self.assertIn(ds.GetRasterBand(1).Checksum(), [4794, 4787])
      self.driver.Delete(out_path)

  def test_jpeg_8(self):
    with gdal.Open(self.getTestFilePath('rgb_ntf_cmyk.jpg')) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 20385)
      self.assertEqual(
          ds.GetRasterBand(1).GetRasterColorInterpretation(), gdal.GCI_RedBand
      )
      self.assertEqual(ds.GetRasterBand(2).Checksum(), 20865)
      self.assertEqual(
          ds.GetRasterBand(2).GetRasterColorInterpretation(), gdal.GCI_GreenBand
      )
      self.assertEqual(ds.GetRasterBand(3).Checksum(), 19441)
      self.assertEqual(
          ds.GetRasterBand(3).GetRasterColorInterpretation(), gdal.GCI_BlueBand
      )
      self.assertEqual(
          ds.GetMetadataItem('SOURCE_COLOR_SPACE', 'IMAGE_STRUCTURE'), 'CMYK'
      )

  def test_jpeg_9(self):
    with gdal.config_option('GDAL_JPEG_TO_RGB', 'NO'):
      with gdal.Open(self.getTestFilePath('rgb_ntf_cmyk.jpg')) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 21187)
        self.assertEqual(
            ds.GetRasterBand(1).GetRasterColorInterpretation(),
            gdal.GCI_CyanBand,
        )
        self.assertEqual(ds.GetRasterBand(2).Checksum(), 21054)
        self.assertEqual(
            ds.GetRasterBand(2).GetRasterColorInterpretation(),
            gdal.GCI_MagentaBand,
        )
        self.assertEqual(ds.GetRasterBand(3).Checksum(), 21499)
        self.assertEqual(
            ds.GetRasterBand(3).GetRasterColorInterpretation(),
            gdal.GCI_YellowBand,
        )
        self.assertEqual(ds.GetRasterBand(4).Checksum(), 21069)
        self.assertEqual(
            ds.GetRasterBand(4).GetRasterColorInterpretation(),
            gdal.GCI_BlackBand,
        )

  def test_jpeg_10(self):
    creation_dts = self.driver.GetMetadataItem(gdal.DMD_CREATIONDATATYPES) or ''
    if 'UInt16' not in creation_dts:
      self.skipTest('12bit jpeg not available')

    with gdal.Open(self.getTestFilePath('12bit_rose_extract.jpg')) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
      stats = ds.GetRasterBand(1).GetStatistics(0, 1)
      self.assertGreaterEqual(stats[2], 3613)
      self.assertLessEqual(stats[2], 3614)

  def test_jpeg_11(self):
    creation_dts = self.driver.GetMetadataItem(gdal.DMD_CREATIONDATATYPES) or ''
    if 'UInt16' not in creation_dts:
      self.skipTest('12bit jpeg not available')

    out_path = '/vsimem/test_jpeg_11.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(self.getTestFilePath('12bit_rose_extract.jpg')) as ds:
        out_ds = self.driver.CreateCopy(out_path, ds)
        del out_ds
      with gdal.Open(out_path) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
        stats = ds.GetRasterBand(1).GetStatistics(0, 1)
        self.assertGreaterEqual(stats[2], 3613)
        self.assertLessEqual(stats[2], 3614)
      self.driver.Delete(out_path)

  def test_jpeg_12(self):
    with gdal.Open('/vsizip/' + self.getTestFilePath('byte_jpg.zip')) as ds:
      self.assertIsNotNone(ds)
      gdal.ErrorReset()
      ds.GetRasterBand(1).Checksum()
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      gdal.ErrorReset()
      ds.GetRasterBand(1).GetMaskBand().Checksum()
      self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_jpeg_13(self):
    out_path = '/vsimem/test_jpeg_13.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        ds = self.driver.CreateCopy(f'/vsistdout_redirect/{out_path}', src_ds)
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
        ds.ReadRaster(0, 0, 1, 1)
        ds = None

      with gdal.Open(out_path) as ds:
        self.assertIsNotNone(ds)

  def test_jpeg_14(self):
    creation_dts = self.driver.GetMetadataItem(gdal.DMD_CREATIONDATATYPES) or ''
    if 'UInt16' not in creation_dts:
      self.skipTest('12bit jpeg not available')

    out_path = '/vsimem/test_jpeg_14.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(self.getTestFilePath('12bit_rose_extract.jpg')) as src_ds:
        ds = self.driver.CreateCopy(f'/vsistdout_redirect/{out_path}', src_ds)
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
        ds = None

      with gdal.Open(out_path) as ds:
        self.assertIsNotNone(ds)

  def test_jpeg_15(self):
    tmp_tif = '/vsimem/test_jpeg_15.tif'
    tmp_jpg = '/vsimem/test_jpeg_15.jpg'
    with (
        gcore_util.GdalUnlinkWhenDone(tmp_tif),
        gcore_util.GdalUnlinkWhenDone(tmp_jpg),
    ):
      gdal.Translate(tmp_tif, self.getTestFilePath('albania.jpg'))
      with gdal.Open(tmp_tif) as src_ds:
        with gdal.quiet_errors():
          new_ds = self.driver.CreateCopy(
              tmp_jpg,
              src_ds,
              callback=lambda pct, msg, user_data: pct <= 0.5,
          )
        self.assertIsNone(new_ds)

  def test_jpeg_16(self):
    tmpdir = self.create_tempdir().full_path
    jpg_path = os.path.join(tmpdir, 'albania.jpg')
    ovr_path = jpg_path + '.ovr'
    shutil.copy(self.getTestFilePath('albania.jpg'), jpg_path)

    with gdal.Open(jpg_path) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
      self.assertIsNone(ds.GetRasterBand(1).GetOverview(-1))
      self.assertIsNone(ds.GetRasterBand(1).GetOverview(1))
      self.assertIsNotNone(ds.GetRasterBand(1).GetOverview(0))
      # Internal overview.
      self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 31892)

      # Build external overviews.
      ds.BuildOverviews('NEAR', [2, 4])
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 2)
      self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 32460)

    # Check external overviews on reopen.
    with gdal.Open(jpg_path) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 2)
      self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 32460)

    self.assertTrue(os.path.exists(ovr_path))
    os.unlink(jpg_path)
    os.unlink(ovr_path)

  def test_jpeg_17(self):
    gdal.ErrorReset()
    with gdal.quiet_errors():
      ds = gdal.Open(self.getTestFilePath('bogus.jpg'))
      self.assertIsNone(ds)
      self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Failure)
      self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.Open(self.getTestFilePath('byte_corrupted.jpg')) as ds:
      with gdal.quiet_errors():
        cs = ds.GetRasterBand(1).Checksum()
    if gdal.GetLastErrorType() != gdal.CE_Failure or not gdal.GetLastErrorMsg():
      self.assertEqual(cs, 4925)

    gdal.ErrorReset()
    with gdal.Open(self.getTestFilePath('byte_corrupted2.jpg')) as ds:
      with gdal.quiet_errors():
        self.assertLess(ds.GetRasterBand(1).Checksum(), 0)
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Failure)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.Open(self.getTestFilePath('byte_corrupted2.jpg')) as ds:
      with gdal.quiet_errors():
        with gdal.config_option('GDAL_ERROR_ON_LIBJPEG_WARNING', 'TRUE'):
          self.assertLess(ds.GetRasterBand(1).Checksum(), 0)
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Failure)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.Open(self.getTestFilePath('byte_corrupted2.jpg')) as ds:
      with gdal.quiet_errors():
        with gdal.config_option('GDAL_ERROR_ON_LIBJPEG_WARNING', 'FALSE'):
          self.assertNotEqual(ds.GetRasterBand(1).Checksum(), 0)
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    corrupted3 = self.getTestFilePath('byte_corrupted3.jpg')
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.Open(corrupted3) as ds:
        self.assertNotEqual(ds.GetRasterBand(1).Checksum(), 0)
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.config_option('GDAL_ERROR_ON_LIBJPEG_WARNING', 'TRUE'):
        ds = gdal.Open(corrupted3)
        del ds
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Failure)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.config_option('GDAL_ERROR_ON_LIBJPEG_WARNING', 'FALSE'):
        with gdal.Open(corrupted3) as ds:
          self.assertNotEqual(ds.GetRasterBand(1).Checksum(), 0)
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_jpeg_18(self):
    height = 1024
    width = 1024
    tif_path = '/vsimem/test_jpeg_18.tif'
    jpg_path = '/vsimem/test_jpeg_18.jpg'
    with (
        gcore_util.GdalUnlinkWhenDone(tif_path),
        gcore_util.GdalUnlinkWhenDone(jpg_path),
    ):
      with gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER).Create(
          tif_path, width, height, 1
      ) as src_ds:
        for row in range(height):
          data = struct.pack('B', int(row / (height / 256)))
          src_ds.WriteRaster(0, row, width, 1, data, 1, 1)
        ds = self.driver.CreateCopy(jpg_path, src_ds, options=['QUALITY=99'])

      gdal.Unlink(tif_path)

      old_cache = gdal.GetCacheMax()
      gdal.SetCacheMax(0)
      try:
        line0 = ds.GetRasterBand(1).ReadRaster(0, 0, width, 1)
        data = struct.unpack('B' * width, line0)
        self.assertAlmostEqual(data[0], 0, delta=10)

        line1023 = ds.GetRasterBand(1).ReadRaster(0, height - 1, width, 1)
        data = struct.unpack('B' * width, line1023)
        self.assertAlmostEqual(data[0], 255, delta=10)

        ovr1 = ds.GetRasterBand(1).GetOverview(1)
        ovr_width = width // 4
        line0_ovr1 = ovr1.ReadRaster(0, 0, ovr_width, 1)
        data = struct.unpack('B' * ovr_width, line0_ovr1)
        self.assertAlmostEqual(data[0], 0, delta=10)

        line1023_bis = ds.GetRasterBand(1).ReadRaster(0, height - 1, width, 1)
        self.assertNotEqual(line1023_bis, line0)
        self.assertEqual(line1023, line1023_bis)

        line0_bis = ds.GetRasterBand(1).ReadRaster(0, 0, width, 1)
        self.assertEqual(line0, line0_bis)

        line255_ovr1 = ovr1.ReadRaster(0, height // 4 - 1, ovr_width, 1)
        data = struct.unpack('B' * ovr_width, line255_ovr1)
        self.assertAlmostEqual(data[0], 255, delta=10)

        line0_bis = ds.GetRasterBand(1).ReadRaster(0, 0, width, 1)
        self.assertEqual(line0, line0_bis)

        line0_ovr1_bis = ovr1.ReadRaster(0, 0, ovr_width, 1)
        self.assertEqual(line0_ovr1, line0_ovr1_bis)

        line255_ovr1_bis = ovr1.ReadRaster(0, height // 4 - 1, ovr_width, 1)
        self.assertEqual(line255_ovr1, line255_ovr1_bis)
      finally:
        ds = None
        gdal.SetCacheMax(old_cache)

  @parameterized.parameters((32, 32, 12), (25, 25, 8), (24, 25, 8))
  def test_jpeg_19(self, width: int, height: int, mask_x: int):
    tif_path = '/vsimem/test_jpeg_19.tif'
    lsb_path = '/vsimem/test_jpeg_19.jpg'
    msb_path = '/vsimem/test_jpeg_19_msb.jpg'
    with (
        gcore_util.GdalUnlinkWhenDone(tif_path),
        gcore_util.GdalUnlinkWhenDone(lsb_path),
        gcore_util.GdalUnlinkWhenDone(msb_path),
    ):
      with gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER).Create(
          tif_path, width, height, 1
      ) as src_ds:
        src_ds.CreateMaskBand(gdal.GMF_PER_DATASET)
        src_ds.GetRasterBand(1).GetMaskBand().WriteRaster(
            0, 0, mask_x, height, struct.pack('B', 255), 1, 1
        )
        src_ds.GetRasterBand(1).GetMaskBand().WriteRaster(
            mask_x, 0, width - mask_x, height, struct.pack('B', 0), 1, 1
        )
        tiff_mask_data = (
            src_ds.GetRasterBand(1)
            .GetMaskBand()
            .ReadRaster(0, 0, width, height)
        )

        # Generate a JPEG file with default LSB bit mask order.
        out_ds = self.driver.CreateCopy(lsb_path, src_ds)
        del out_ds

        # Generate a JPEG file with MSB bit mask order.
        with gdal.config_option('JPEG_WRITE_MASK_BIT_ORDER', 'MSB'):
          out_ds = self.driver.CreateCopy(msb_path, src_ds)
          del out_ds

      data_lsb = gcore_util.vsi_read_file_as_bytes(
          lsb_path, max_size=gdal.VSIStatL(lsb_path).size
      )
      data_msb = gcore_util.vsi_read_file_as_bytes(
          msb_path, max_size=gdal.VSIStatL(msb_path).size
      )
      if (width, height, mask_x) == (24, 25, 8):
        self.assertEqual(data_lsb, data_msb)
      else:
        self.assertNotEqual(data_lsb, data_msb)

      with gdal.Open(lsb_path) as ds:
        jpg_mask_data = (
            ds.GetRasterBand(1).GetMaskBand().ReadRaster(0, 0, width, height)
        )
      self.assertEqual(tiff_mask_data, jpg_mask_data)

      with gdal.Open(msb_path) as ds:
        jpg_mask_data = (
            ds.GetRasterBand(1).GetMaskBand().ReadRaster(0, 0, width, height)
        )
      self.assertEqual(tiff_mask_data, jpg_mask_data)

  def test_jpeg_mask_lsb_order_issue_4351(self):
    src_ds = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER).Create('', 15, 4, 3)
    src_ds.CreateMaskBand(gdal.GMF_PER_DATASET)
    src_ds.GetRasterBand(1).GetMaskBand().WriteRaster(7, 2, 2, 1, b'\xff' * 2)
    tmpfilename = '/vsimem/test_jpeg_mask_lsb_order_issue_4351.jpg'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
      self.assertIsNotNone(out_ds)
      del out_ds
      with gdal.Open(tmpfilename) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMaskBand().ReadRaster(0, 2, 15, 1),
            b'\x00' * 7 + b'\xff' * 2 + b'\x00' * 6,
        )

  def test_jpeg_20(self):
    rgbsmall_path = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="50" rasterYSize="50">\n'
        '  <GCPList Projection="WGS84">\n'
        '    <GCP Id="1" Pixel="0" Line="0" X="0" Y="0"/>\n'
        '    <GCP Id="2" Pixel="0" Line="50" X="0" Y="50"/>\n'
        '    <GCP Id="3" Pixel="50" Line="50" X="50" Y="50"/>\n'
        '    <GCP Id="4" Pixel="50" Line="0" X="50" Y="0"/>\n'
        '  </GCPList>\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Red</ColorInterp>\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="0">'
        f'{rgbsmall_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="50" ySize="50"/>\n'
        '      <DstRect xOff="0" yOff="0" xSize="50" ySize="50"/>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <ColorInterp>Green</ColorInterp>\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="0">'
        f'{rgbsmall_path}</SourceFilename>\n'
        '      <SourceBand>2</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="50" ySize="50"/>\n'
        '      <DstRect xOff="0" yOff="0" xSize="50" ySize="50"/>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="3">\n'
        '    <ColorInterp>Blue</ColorInterp>\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="0">'
        f'{rgbsmall_path}</SourceFilename>\n'
        '      <SourceBand>3</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="50" ySize="50"/>\n'
        '      <DstRect xOff="0" yOff="0" xSize="50" ySize="50"/>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>\n'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/test_jpeg_20') as vsi_dir:
      vrt_path = f'{vsi_dir}/rgb_gcp.vrt'
      jpg_path = f'{vsi_dir}/jpeg_20.jpg'
      gdal.FileFromMemBuffer(vrt_path, vrt_xml)
      with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
        with gdal.Open(vrt_path) as src_ds:
          out_ds = self.driver.CreateCopy(jpg_path, src_ds)
          del out_ds
        with gdal.Open(jpg_path) as ds:
          self.assertTrue(ds.GetGCPProjection().startswith('GEOGCS["WGS 84"'))
          self.assertEqual(ds.GetGCPCount(), 4)
          self.assertLen(ds.GetGCPs(), 4)

  def test_jpeg_21(self):
    with gdal.Open(self.getTestFilePath('black_with_white_exif_ovr.jpg')) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 3)
      expected_dim_cs = [(512, 512, 0), (256, 256, 0), (196, 196, 12681)]
      for idx, (expected_w, expected_h, expected_cs) in enumerate(
          expected_dim_cs
      ):
        ovr = ds.GetRasterBand(1).GetOverview(idx)
        self.assertEqual(ovr.XSize, expected_w)
        self.assertEqual(ovr.YSize, expected_h)
        self.assertEqual(ovr.Checksum(), expected_cs)

  def test_jpeg_22(self):
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    out_path = '/vsimem/test_jpeg_22.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      src_ds = mem_drv.Create('', 4096, 2048)
      src_ds.GetRasterBand(1).Fill(255)
      with self.driver.CreateCopy(
          out_path, src_ds, options=['EXIF_THUMBNAIL=YES']
      ) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 4)
        ovr = ds.GetRasterBand(1).GetOverview(3)
        self.assertEqual(ovr.XSize, 128)
        self.assertEqual(ovr.YSize, 64)
        self.assertEqual(ovr.Checksum(), 34957)

      # With 3 bands.
      src_ds = mem_drv.Create('', 2048, 4096, 3)
      src_ds.GetRasterBand(1).Fill(255)
      with self.driver.CreateCopy(
          out_path, src_ds, options=['EXIF_THUMBNAIL=YES']
      ) as ds:
        ovr = ds.GetRasterBand(1).GetOverview(3)
        self.assertEqual(ovr.XSize, 64)
        self.assertEqual(ovr.YSize, 128)

      # With comment and THUMBNAIL_WIDTH.
      src_ds = mem_drv.Create('', 2048, 4096)
      src_ds.GetRasterBand(1).Fill(255)
      with self.driver.CreateCopy(
          out_path,
          src_ds,
          options=['COMMENT=foo', 'EXIF_THUMBNAIL=YES', 'THUMBNAIL_WIDTH=40'],
      ) as ds:
        ovr = ds.GetRasterBand(1).GetOverview(3)
        self.assertEqual(ds.GetMetadataItem('COMMENT'), 'foo')
        self.assertEqual(ovr.XSize, 40)
        self.assertEqual(ovr.YSize, 80)

      # With THUMBNAIL_HEIGHT.
      src_ds = mem_drv.Create('', 2048, 4096)
      src_ds.GetRasterBand(1).Fill(255)
      with self.driver.CreateCopy(
          out_path,
          src_ds,
          options=['EXIF_THUMBNAIL=YES', 'THUMBNAIL_HEIGHT=60'],
      ) as ds:
        ovr = ds.GetRasterBand(1).GetOverview(3)
        self.assertEqual(ovr.XSize, 30)
        self.assertEqual(ovr.YSize, 60)

      # With both THUMBNAIL_WIDTH and THUMBNAIL_HEIGHT.
      src_ds = mem_drv.Create('', 2048, 4096)
      src_ds.GetRasterBand(1).Fill(255)
      with self.driver.CreateCopy(
          out_path,
          src_ds,
          options=[
              'EXIF_THUMBNAIL=YES',
              'THUMBNAIL_WIDTH=50',
              'THUMBNAIL_HEIGHT=40',
          ],
      ) as ds:
        ovr = ds.GetRasterBand(1).GetOverview(3)
        self.assertEqual(ovr.XSize, 50)
        self.assertEqual(ovr.YSize, 40)

  def test_jpeg_23(self):
    albania_path = self.getTestFilePath('albania.jpg')
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)

    with gdal.Open(albania_path) as ds:
      cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
      # Band interleaved.
      data = ds.ReadRaster(0, 0, ds.RasterXSize, ds.RasterYSize)
      tmp_ds = mem_drv.Create('', ds.RasterXSize, ds.RasterYSize, 3)
      tmp_ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, data)
      self.assertEqual(
          [tmp_ds.GetRasterBand(i + 1).Checksum() for i in range(3)], cs
      )

    # Pixel interleaved.
    with gdal.Open(albania_path) as ds:
      data = ds.ReadRaster(
          0,
          0,
          ds.RasterXSize,
          ds.RasterYSize,
          buf_pixel_space=3,
          buf_band_space=1,
      )
      half_y = ds.RasterYSize // 2
      data_bottom = ds.ReadRaster(
          0,
          half_y,
          ds.RasterXSize,
          ds.RasterYSize - half_y,
          buf_pixel_space=3,
          buf_band_space=1,
      )
      data_top = ds.ReadRaster(
          0, 0, ds.RasterXSize, half_y, buf_pixel_space=3, buf_band_space=1
      )
      self.assertEqual(data, data_top + data_bottom)
      tmp_ds = mem_drv.Create('', ds.RasterXSize, ds.RasterYSize, 3)
      tmp_ds.WriteRaster(
          0,
          0,
          ds.RasterXSize,
          ds.RasterYSize,
          data,
          buf_pixel_space=3,
          buf_band_space=1,
      )
      self.assertEqual(
          [tmp_ds.GetRasterBand(i + 1).Checksum() for i in range(3)], cs
      )

    # Pixel interleaved with padding.
    with gdal.Open(albania_path) as ds:
      data = ds.ReadRaster(
          0,
          0,
          ds.RasterXSize,
          ds.RasterYSize,
          buf_pixel_space=4,
          buf_band_space=1,
      )
      tmp_ds = mem_drv.Create('', ds.RasterXSize, ds.RasterYSize, 3)
      tmp_ds.WriteRaster(
          0,
          0,
          ds.RasterXSize,
          ds.RasterYSize,
          data,
          buf_pixel_space=4,
          buf_band_space=1,
      )
      self.assertEqual(
          [tmp_ds.GetRasterBand(i + 1).Checksum() for i in range(3)], cs
      )

  def test_jpeg_24(self):
    creation_opts = (
        self.driver.GetMetadataItem(gdal.DMD_CREATIONOPTIONLIST) or ''
    )
    has_arithmetic = 'ARITHMETIC' in creation_opts
    out_path = '/vsimem/test_jpeg_24.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        if not has_arithmetic:
          with gdal.quiet_errors():
            ds = self.driver.CreateCopy(
                out_path, src_ds, options=['ARITHMETIC=YES']
            )
            self.assertIsNone(ds)
          return

        gdal.ErrorReset()
        ds = self.driver.CreateCopy(
            out_path, src_ds, options=['ARITHMETIC=YES']
        )
        if 'Requested feature was omitted at compile time' in (
            gdal.GetLastErrorMsg()
        ):
          del ds
          self.skipTest('Arithmetic coding omitted at compile time')
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4743)
        del ds
      self.driver.Delete(out_path)

  def test_jpeg_25(self):
    out_path = '/vsimem/test_jpeg_25.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        out_ds = self.driver.CreateCopy(
            out_path, src_ds, options=['COMMENT=my comment']
        )
        del out_ds
      with gdal.Open(out_path) as ds:
        self.assertEqual(ds.GetMetadataItem('COMMENT'), 'my comment')
      self.driver.Delete(out_path)

  def testFailCreateCopyOnEmpty26(self):
    out_path = '/vsimem/jpeg_26.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      src = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER).Create('', 70000, 1)
      with gdal.quiet_errors():
        dst = self.driver.CreateCopy(out_path, src)
        self.assertIsNone(dst)

  def test_jpeg_27_max_memory(self):
    zip_path = self.getTestFilePath('eofloop_valid_huff.tif.zip')
    subfile_path = f'/vsisubfile/146,/vsizip/{zip_path}'
    gdal.ErrorReset()
    with (
        mock.patch.dict(os.environ, {'JPEGMEM': '10M'}),
        gdal.quiet_errors(),
        gdal.config_option('GDAL_JPEG_MAX_ALLOWED_SCAN_NUMBER', '1000'),
        gdal.Open(subfile_path) as ds,
    ):
      cs = ds.GetRasterBand(1).Checksum()
    self.assertEqual(cs, -1)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_jpeg_27_max_scan_number(self):
    zip_path = self.getTestFilePath('eofloop_valid_huff.tif.zip')
    subfile_path = f'/vsisubfile/146,/vsizip/{zip_path}'
    gdal.ErrorReset()
    options = {
        'GDAL_ALLOW_LARGE_LIBJPEG_MEM_ALLOC': 'YES',
        'GDAL_JPEG_MAX_ALLOWED_SCAN_NUMBER': '10',
    }
    with gdal.Open(subfile_path) as ds:
      with gdal.config_options(options), gdal.quiet_errors():
        cs = ds.GetRasterBand(1).Checksum()
    self.assertEqual(cs, -1)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_jpeg_28(self):
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/test_jpeg_28') as vsi_dir:
      tmpfilename = f'{vsi_dir}/jpeg_28.jpg'
      with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
        # Nothing.
        src_ds = mem_drv.Create('', 1, 1)
        out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
        del out_ds
        src_ds = None
        with gdal.Open(tmpfilename) as ds:
          self.assertEqual(ds.GetMetadata(), {})

        # EXIF tags only.
        src_ds = mem_drv.Create('', 1, 1)
        src_ds.SetMetadataItem('EXIF_DateTime', 'dt')
        src_ds.SetMetadataItem('EXIF_DateTimeOriginal', '01234567890123456789')
        src_ds.SetMetadataItem('EXIF_DateTimeDigitized', '0123456789012345678')
        src_ds.SetMetadataItem('EXIF_Make', 'make')
        src_ds.SetMetadataItem('EXIF_ExifVersion', '01234')
        src_ds.SetMetadataItem('EXIF_ComponentsConfiguration', '0x1F')
        src_ds.SetMetadataItem('EXIF_FlashpixVersion', 'ABCD')
        src_ds.SetMetadataItem('EXIF_SpatialFrequencyResponse', '0xab 0xCD')
        src_ds.SetMetadataItem('EXIF_Orientation', '10')
        src_ds.SetMetadataItem('EXIF_ResolutionUnit', '2 4')
        src_ds.SetMetadataItem('EXIF_TransferFunction', '0 1')
        src_ds.SetMetadataItem('EXIF_ISOSpeedRatings', '1 2 3')
        src_ds.SetMetadataItem('EXIF_StandardOutputSensitivity', '123456789')
        src_ds.SetMetadataItem('EXIF_XResolution', '96')
        src_ds.SetMetadataItem('EXIF_YResolution', '96 0')
        src_ds.SetMetadataItem('EXIF_CompressedBitsPerPixel', 'nan')
        src_ds.SetMetadataItem('EXIF_ApertureValue', '-1')

        with gdal.quiet_errors():
          out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
          del out_ds
        src_ds = None
        self.assertIsNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
        with gdal.Open(tmpfilename) as ds:
          expected_md = {
              'EXIF_DateTimeDigitized': '0123456789012345678',
              'EXIF_DateTimeOriginal': '0123456789012345678',
              'EXIF_Orientation': '10',
              'EXIF_ApertureValue': '(0)',
              'EXIF_YResolution': '(96)',
              'EXIF_XResolution': '(96)',
              'EXIF_TransferFunction': '0 1' + ' 0' * 766,
              'EXIF_ExifVersion': '0123',
              'EXIF_DateTime': 'dt',
              'EXIF_FlashpixVersion': 'ABCD',
              'EXIF_ComponentsConfiguration': '0x1f 0x00 0x00 0x00',
              'EXIF_Make': 'make',
              'EXIF_StandardOutputSensitivity': '123456789',
              'EXIF_ResolutionUnit': '2',
              'EXIF_CompressedBitsPerPixel': '(0)',
              'EXIF_SpatialFrequencyResponse': '0xab 0xcd',
              'EXIF_ISOSpeedRatings': '1 2 3',
          }
          self.assertEqual(ds.GetMetadata(), expected_md)

        # Test SRATIONAL.
        for val in (-1.5, -1, -0.5, 0, 0.5, 1, 1.5):
          src_ds = mem_drv.Create('', 1, 1)
          src_ds.SetMetadataItem('EXIF_ShutterSpeedValue', str(val))
          out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
          del out_ds
          src_ds = None
          self.assertIsNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
          with gdal.Open(tmpfilename) as ds:
            got_val = ds.GetMetadataItem('EXIF_ShutterSpeedValue')
            got_val = got_val.replace('(', '').replace(')', '')
            self.assertEqual(float(got_val), val)

        # Test RATIONAL.
        for val in (0, 0.5, 1, 1.5):
          src_ds = mem_drv.Create('', 1, 1)
          src_ds.SetMetadataItem('EXIF_ApertureValue', str(val))
          out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
          del out_ds
          src_ds = None
          self.assertIsNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
          with gdal.Open(tmpfilename) as ds:
            got_val = ds.GetMetadataItem('EXIF_ApertureValue')
            got_val = got_val.replace('(', '').replace(')', '')
            self.assertEqual(float(got_val), val)

        # GPS tags only.
        src_ds = mem_drv.Create('', 1, 1)
        src_ds.SetMetadataItem('EXIF_GPSLatitudeRef', 'N')
        src_ds.SetMetadataItem('EXIF_GPSLatitude', '49 34 56.5')
        out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
        del out_ds
        src_ds = None
        self.assertIsNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
        with gdal.Open(tmpfilename) as ds:
          self.assertEqual(
              ds.GetMetadata(),
              {
                  'EXIF_GPSLatitudeRef': 'N',
                  'EXIF_GPSLatitude': '(49) (34) (56.5)',
              },
          )

        # EXIF and GPS tags.
        src_ds = mem_drv.Create('', 1, 1)
        src_ds.SetMetadataItem('EXIF_ExifVersion', '0231')
        src_ds.SetMetadataItem('EXIF_GPSLatitudeRef', 'N')
        out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
        del out_ds
        src_ds = None
        self.assertIsNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
        with gdal.Open(tmpfilename) as ds:
          self.assertEqual(
              ds.GetMetadata(),
              {'EXIF_ExifVersion': '0231', 'EXIF_GPSLatitudeRef': 'N'},
          )

        # EXIF and other metadata.
        src_ds = mem_drv.Create('', 1, 1)
        src_ds.SetMetadataItem('EXIF_ExifVersion', '0231')
        src_ds.SetMetadataItem('EXIF_invalid', 'foo')
        src_ds.SetMetadataItem('FOO', 'BAR')
        with gdal.quiet_errors():
          out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
          del out_ds
        src_ds = None
        self.assertIsNotNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
        with gdal.Open(tmpfilename) as ds:
          self.assertEqual(
              ds.GetMetadata(),
              {
                  'EXIF_ExifVersion': '0231',
                  'EXIF_invalid': 'foo',
                  'FOO': 'BAR',
              },
          )

        # Too much content for EXIF.
        src_ds = mem_drv.Create('', 1, 1)
        src_ds.SetMetadataItem('EXIF_UserComment', 'x' * 65535)
        with gdal.quiet_errors():
          out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
          del out_ds
        src_ds = None

        # EXIF, GPS tags, and EXIF overview.
        src_ds = mem_drv.Create('', 1024, 1024)
        src_ds.SetMetadataItem('EXIF_ExifVersion', '0231')
        src_ds.SetMetadataItem('EXIF_GPSLatitudeRef', 'N')
        out_ds = self.driver.CreateCopy(
            tmpfilename,
            src_ds,
            options=[
                'EXIF_THUMBNAIL=YES',
                'THUMBNAIL_WIDTH=32',
                'THUMBNAIL_HEIGHT=32',
            ],
        )
        del out_ds
        src_ds = None
        self.assertIsNone(gdal.VSIStatL(tmpfilename + '.aux.xml'))
        with gdal.Open(tmpfilename) as ds:
          self.assertEqual(
              ds.GetMetadata(),
              {'EXIF_ExifVersion': '0231', 'EXIF_GPSLatitudeRef': 'N'},
          )
          band = ds.GetRasterBand(1)
          self.assertEqual(
              band.GetOverview(band.GetOverviewCount() - 1).XSize, 32
          )

  def test_jpeg_multiscan_overviews(self):
    tmpfilename = '/vsimem/test_jpeg_multiscan_overviews.jpg'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      src_ds = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER).Create(
          '', 10000, 1000
      )
      src_ds.GetRasterBand(1).Fill(255)
      out_ds = self.driver.CreateCopy(
          tmpfilename, src_ds, options=['PROGRESSIVE=YES']
      )
      del out_ds
      src_ds = None
      with gdal.Open(tmpfilename) as ds:
        band = ds.GetRasterBand(1)
        for row in (0, 1):
          self.assertEqual(
              struct.unpack('B', band.ReadRaster(0, row, 1, 1))[0], 255
          )
          self.assertEqual(
              struct.unpack('B', band.GetOverview(0).ReadRaster(0, row, 1, 1))[
                  0
              ],
              255,
          )
          self.assertEqual(
              struct.unpack('B', band.GetOverview(1).ReadRaster(0, row, 1, 1))[
                  0
              ],
              255,
          )

  def test_jpeg_flir_png(self):
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    png_drv = gdal.GetDriverByName(gdrivers_util.PNG_DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/test_jpeg_flir_png') as vsi_dir:
      png_path = f'{vsi_dir}/inner.png'
      jpg_path = f'{vsi_dir}/flir.jpg'

      png_mem = mem_drv.Create('', 4, 4, 3, gdal.GDT_Byte)
      for band_idx in range(1, 4):
        png_mem.GetRasterBand(band_idx).Fill(band_idx * 30)
      expected_cs = png_mem.GetRasterBand(1).Checksum()
      out_png = png_drv.CreateCopy(png_path, png_mem)
      del out_png
      png_mem = None
      png_bytes = gcore_util.vsi_read_file_as_bytes(
          png_path, max_size=gdal.VSIStatL(png_path).size
      )

      gdal.FileFromMemBuffer(jpg_path, _build_flir_jpeg_bytes(4, 4, png_bytes))
      with gdal.Open(jpg_path) as ds:
        self.assertEqual(
            ds.GetMetadataDomainList(),
            [
                'IMAGE_STRUCTURE',
                '',
                'FLIR',
                'SUBDATASETS',
                'DERIVED_SUBDATASETS',
            ],
        )
        flir_md = ds.GetMetadata('FLIR')
        self.assertEqual(flir_md['CreatorSoftware'], 'TestCreator')
        self.assertEqual(flir_md['CameraModel'], 'FLIR_A65')
        self.assertEqual(flir_md['PaletteName'], 'Iron')
        self.assertEqual(flir_md['Palette'], '(10 20 30), (40 50 60)')
        self.assertEqual(flir_md['Emissivity'], '0.950000')
        self.assertEqual(flir_md['ObjectDistance'], '2.000000 m')
        self.assertEqual(flir_md['ReflectedApparentTemperature'], '20.000000 C')
        self.assertEqual(flir_md['RelativeHumidity'], '50.000000 %')
        self.assertEqual(flir_md['RawThermalImageWidth'], '4')
        self.assertEqual(flir_md['RawThermalImageHeight'], '4')
        subds = ds.GetSubDatasets()
        self.assertLen(subds, 1)

      with gdal.Open(subds[0][0]) as sub_ds:
        self.assertIsNotNone(sub_ds)
        self.assertEqual(sub_ds.RasterCount, 3)
        self.assertEqual(sub_ds.GetRasterBand(1).Checksum(), expected_cs)

  def test_jpeg_flir_png_16_bit(self):
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    png_drv = gdal.GetDriverByName(gdrivers_util.PNG_DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/test_jpeg_flir_png_16'
    ) as vsi_dir:
      png_path = f'{vsi_dir}/inner_16.png'
      jpg_path = f'{vsi_dir}/tmp.jpg'

      png_mem = mem_drv.Create('', 2, 2, 1, gdal.GDT_UInt16)
      # 255 (0x00FF) byte-swaps under @BYTE_ORDER_LITTLE_ENDIAN=YES to 65280.
      png_mem.GetRasterBand(1).Fill(255)
      out_png = png_drv.CreateCopy(png_path, png_mem)
      del out_png
      png_mem = None
      png_bytes = gcore_util.vsi_read_file_as_bytes(
          png_path, max_size=gdal.VSIStatL(png_path).size
      )

      gdal.FileFromMemBuffer(jpg_path, _build_flir_jpeg_bytes(2, 2, png_bytes))
      subds_name = f'JPEG:"{jpg_path}":FLIR_RAW_THERMAL_IMAGE'

      with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
        with gdal.Open(subds_name) as ds:
          self.assertEqual(ds.GetDescription(), subds_name)
          self.assertEqual(ds.GetFileList(), [jpg_path])
          self.assertEqual(ds.GetDriver().GetDescription(), 'PNG')
          self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
          self.assertEqual(
              ds.GetRasterBand(1).GetStatistics(False, True),
              [65280.0, 65280.0, 65280.0, 0.0],
          )

        self.assertIsNotNone(gdal.VSIStatL(jpg_path + '.aux.xml'))

        gdal.FileFromMemBuffer(
            jpg_path + '.aux.xml',
            '<PAMDataset>\n'
            '  <Subdataset name="PNG_THERMAL_IMAGE">\n'
            '    <PAMDataset>\n'
            '      <PAMRasterBand band="1">\n'
            '        <Metadata>\n'
            '          <MDI key="STATISTICS_MINIMUM">1</MDI>\n'
            '          <MDI key="STATISTICS_MAXIMUM">2</MDI>\n'
            '          <MDI key="STATISTICS_MEAN">3</MDI>\n'
            '          <MDI key="STATISTICS_STDDEV">4</MDI>\n'
            '          <MDI key="STATISTICS_VALID_PERCENT">100</MDI>\n'
            '        </Metadata>\n'
            '      </PAMRasterBand>\n'
            '    </PAMDataset>\n'
            '  </Subdataset>\n'
            '</PAMDataset>',
        )

        with gdal.Open(subds_name) as ds:
          self.assertEqual(ds.GetFileList(), [jpg_path, jpg_path + '.aux.xml'])
          self.assertEqual(
              ds.GetRasterBand(1).GetStatistics(False, True), [1, 2, 3, 4]
          )

  def test_jpeg_flir_raw(self):
    with gcore_util.VsimemCleanupDir('/vsimem/test_jpeg_flir_raw') as vsi_dir:
      jpg_path = f'{vsi_dir}/tmp.jpg'
      raw_values = [500 + idx for idx in range(8)]
      raw_payload = struct.pack('<8H', *raw_values)
      gdal.FileFromMemBuffer(
          jpg_path, _build_flir_jpeg_bytes(4, 2, raw_payload)
      )

      with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
        with gdal.Open(jpg_path) as ds:
          subds = ds.GetSubDatasets()
          self.assertLen(subds, 1)

        with gdal.Open(subds[0][0]) as ds:
          self.assertIsNotNone(ds)
          self.assertEqual(ds.GetDescription(), subds[0][0])
          self.assertEqual(ds.GetFileList(), [jpg_path])
          self.assertEqual(ds.RasterCount, 1)
          self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
          stats = ds.GetRasterBand(1).GetStatistics(False, True)
          self.assertEqual(stats[0], 500.0)
          self.assertEqual(stats[1], 507.0)
          self.assertAlmostEqual(stats[2], 503.5, delta=1e-6)

        self.assertIsNotNone(gdal.VSIStatL(jpg_path + '.aux.xml'))

        gdal.FileFromMemBuffer(
            jpg_path + '.aux.xml',
            '<PAMDataset>\n'
            '  <Subdataset name="RAW_THERMAL_IMAGE">\n'
            '    <PAMDataset>\n'
            '      <PAMRasterBand band="1">\n'
            '        <Metadata>\n'
            '          <MDI key="STATISTICS_MINIMUM">1</MDI>\n'
            '          <MDI key="STATISTICS_MAXIMUM">2</MDI>\n'
            '          <MDI key="STATISTICS_MEAN">3</MDI>\n'
            '          <MDI key="STATISTICS_STDDEV">4</MDI>\n'
            '          <MDI key="STATISTICS_VALID_PERCENT">100</MDI>\n'
            '        </Metadata>\n'
            '      </PAMRasterBand>\n'
            '    </PAMDataset>\n'
            '  </Subdataset>\n'
            '</PAMDataset>',
        )

        with gdal.Open(subds[0][0]) as ds:
          self.assertEqual(ds.GetFileList(), [jpg_path, jpg_path + '.aux.xml'])
          self.assertEqual(
              ds.GetRasterBand(1).GetStatistics(False, True), [1, 2, 3, 4]
          )

  def test_jpeg_flir_error_flir_subds(self):
    with gdal.quiet_errors():
      self.assertIsNone(gdal.Open('JPEG:foo.jpg'))
      self.assertIsNone(gdal.Open('JPEG:foo.jpg:BAR'))
      self.assertIsNone(
          gdal.Open(
              f'JPEG:{self.getTestFilePath("masked.jpg")}:FLIR_RAW_THERMAL_IMAGE'
          )
      )

  def test_jpeg_dji_thermal_metadata(self):
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    width, height = 16, 8
    raw_values = [1000 + idx for idx in range(width * height)]
    raw_bytes = struct.pack(f'<{width * height}H', *raw_values)
    mem_ds = mem_drv.Create('', width, height, 1, gdal.GDT_UInt16)
    mem_ds.GetRasterBand(1).WriteRaster(0, 0, width, height, raw_bytes)
    expected_cs = mem_ds.GetRasterBand(1).Checksum()
    mem_ds = None

    with gcore_util.VsimemCleanupDir('/vsimem/test_jpeg_dji_meta') as vsi_dir:
      jpg_path = f'{vsi_dir}/test.jpg'
      gdal.FileFromMemBuffer(
          jpg_path, _build_dji_jpeg_bytes(width, height, raw_bytes)
      )

      with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
        with gdal.Open(jpg_path) as ds:
          self.assertEqual(
              sorted(ds.GetMetadataDomainList()),
              [
                  '',
                  'DERIVED_SUBDATASETS',
                  'DJI',
                  'IMAGE_STRUCTURE',
                  'SUBDATASETS',
                  'xml:XMP',
              ],
          )
          self.assertEqual(
              ds.GetMetadata('DJI'),
              {
                  'RawThermalImageHeight': str(height),
                  'RawThermalImageWidth': str(width),
              },
          )
          self.assertEqual(ds.RasterCount, 3)
          subds = ds.GetSubDatasets()
          self.assertLen(subds, 1)

        self.assertIsNone(gdal.VSIStatL(jpg_path + '.aux.xml'))

        with gdal.Open(subds[0][0]) as ds:
          self.assertIsNotNone(ds)
          self.assertEqual(ds.RasterCount, 1)
          self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
          self.assertEqual(ds.GetRasterBand(1).Checksum(), expected_cs)

  def test_jpeg_dji_thermal_raw(self):
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    width, height = 16, 8
    raw_values = [1000 + idx for idx in range(width * height)]
    raw_bytes = struct.pack(f'<{width * height}H', *raw_values)
    mem_ds = mem_drv.Create('', width, height, 1, gdal.GDT_UInt16)
    mem_ds.GetRasterBand(1).WriteRaster(0, 0, width, height, raw_bytes)
    expected_cs = mem_ds.GetRasterBand(1).Checksum()
    mem_ds = None

    with gcore_util.VsimemCleanupDir('/vsimem/test_jpeg_dji_raw') as vsi_dir:
      jpg_path = f'{vsi_dir}/dji_raw.jpg'
      gdal.FileFromMemBuffer(
          jpg_path, _build_dji_jpeg_bytes(width, height, raw_bytes)
      )
      with gdal.Open(f'JPEG:"{jpg_path}":DJI_RAW_THERMAL_IMAGE') as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), expected_cs)

  def test_jpeg_write_cmyk(self):
    out_path = '/vsimem/test_jpeg_write_cmyk.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      with gdal.config_option('GDAL_JPEG_TO_RGB', 'NO'):
        src_ds = gdal.Open(self.getTestFilePath('rgb_ntf_cmyk.jpg'))
      gdal.ErrorReset()
      out_ds = self.driver.CreateCopy(out_path, src_ds)
      del out_ds
      src_ds = None
      self.assertEqual(gdal.GetLastErrorMsg(), '')

      with gdal.config_option('GDAL_JPEG_TO_RGB', 'NO'):
        with gdal.Open(out_path) as ds:
          self.assertEqual(
              ds.GetRasterBand(1).GetRasterColorInterpretation(),
              gdal.GCI_CyanBand,
          )

  def test_jpeg_write_4band_not_cmyk(self):
    out_path = '/vsimem/test_jpeg_write_4band_not_cmyk.jpg'
    with gcore_util.GdalUnlinkWhenDone(out_path):
      src_ds = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER).Create(
          '', 8, 8, 4
      )
      gdal.ErrorReset()
      with gdal.quiet_errors():
        out_ds = self.driver.CreateCopy(out_path, src_ds)
        del out_ds
      self.assertNotEqual(gdal.GetLastErrorMsg(), '')
      self.driver.Delete(out_path)

  @parameterized.parameters(*range(1, 9))
  def test_jpeg_apply_orientation(self, orientation: int):
    filepath = self.getTestFilePath(f'exif_orientation/F{orientation}.jpg')
    with gdal.OpenEx(filepath, open_options=['APPLY_ORIENTATION=YES']) as ds:
      self.assertEqual(ds.RasterXSize, 3)
      self.assertEqual(ds.RasterYSize, 5)
      vals = struct.unpack('B' * 3 * 5, ds.ReadRaster())
      vals = [1 if v else 0 for v in vals]
      self.assertEqual(vals, [1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0])
      self.assertEqual(
          ds.GetRasterBand(1).GetColorInterpretation(), gdal.GCI_GrayIndex
      )
      if orientation != 1:
        self.assertIsNone(ds.GetMetadataItem('EXIF_Orientation'))
        self.assertEqual(
            ds.GetMetadataItem('original_EXIF_Orientation'), str(orientation)
        )

  def test_jpeg_from_jpegxl(self):
    jpegxl_drv = gdal.GetDriverByName(gdrivers_util.JPEGXL_DRIVER)
    if jpegxl_drv is None:
      self.skipTest('JPEGXL driver missing')
    jxl_creation_opts = (
        jpegxl_drv.GetMetadataItem(gdal.DMD_CREATIONOPTIONLIST) or ''
    )
    if 'COMPRESS_BOXES' not in jxl_creation_opts:
      self.skipTest('JPEGXL COMPRESS_BOXES creation option missing')

    tmp_jxl = '/vsimem/test_jpeg_from_jpegxl.jxl'
    out_jpg = '/vsimem/test_jpeg_from_jpegxl_out.jpg'
    with (
        gcore_util.GdalUnlinkWhenDone(tmp_jxl),
        gcore_util.GdalUnlinkWhenDone(out_jpg),
        gcore_util.GdalUnlinkWhenDone(out_jpg + '.aux.xml'),
    ):
      with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
        src_ds = gdal.Open(self.getTestFilePath('albania.jpg'))
        src_cs = [src_ds.GetRasterBand(b).Checksum() for b in (1, 2, 3)]

        # Lossless JPEG -> JPEGXL conversion.
        tmp_ds = jpegxl_drv.CreateCopy(tmp_jxl, src_ds)
        src_ds = None

        # Lossless JPEGXL -> JPEG conversion.
        out_ds = gdal.Translate(out_jpg, tmp_ds, metadataOptions=['foo=bar'])
        tmp_ds = None

        for idx, expected_cs in enumerate(src_cs, start=1):
          self.assertEqual(out_ds.GetRasterBand(idx).Checksum(), expected_cs)
        self.assertEqual(out_ds.GetMetadataItem('EXIF_ExifVersion'), '0210')
        out_ds = None

        # Check data is preserved after file reopening.
        with gdal.Open(out_jpg) as ds:
          for idx, expected_cs in enumerate(src_cs, start=1):
            self.assertEqual(ds.GetRasterBand(idx).Checksum(), expected_cs)
          self.assertEqual(ds.GetMetadataItem('EXIF_ExifVersion'), '0210')
          self.assertEqual(ds.GetMetadataItem('foo'), 'bar')

        gdal.Unlink(out_jpg + '.aux.xml')
        with gdal.Open(out_jpg) as ds:
          self.assertEqual(ds.GetMetadataItem('EXIF_ExifVersion'), '0210')

  def test_jpeg_read_arcgis_geodataxform_gcp(self):
    with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
      with gdal.Open(self.getTestFilePath('arcgis_geodataxform_gcp.jpg')) as ds:
        self.assertIn('26712', ds.GetGCPProjection())
        self.assertEqual(ds.GetGCPCount(), 4)
        gcp = ds.GetGCPs()[0]
        self.assertAlmostEqual(gcp.GCPPixel, 565, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPLine, 11041, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPX, 500000, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPY, 4705078.79016612, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPZ, 0, delta=1e-5)

  def test_jpeg_read_arcgis_metadata_geodataxform_gcp(self):
    with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
      with gdal.Open(
          self.getTestFilePath('arcgis_metadata_geodataxform_gcp.jpg')
      ) as ds:
        self.assertIn('26712', ds.GetGCPProjection())
        self.assertEqual(ds.GetGCPCount(), 4)
        gcp = ds.GetGCPs()[0]
        self.assertAlmostEqual(gcp.GCPPixel, 565, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPLine, 11041, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPX, 500000, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPY, 4705078.79016612, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPZ, 0, delta=1e-5)

  def test_jpeg_read_lossless(self):
    if self.driver.GetMetadataItem('LOSSLESS_JPEG_SUPPORTED', 'JPEG') is None:
      self.skipTest('lossless jpeg not supported')
    with gdal.Open(self.getTestFilePath('byte_lossless.jpg')) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(
          ds.GetMetadataItem('COMPRESSION_REVERSIBILITY', 'IMAGE_STRUCTURE'),
          'LOSSLESS',
      )

  def test_jpeg_read_lossless_16bit(self):
    if self.driver.GetMetadataItem('LOSSLESS_JPEG_SUPPORTED', 'JPEG') is None:
      self.skipTest('lossless jpeg not supported')
    with gdal.quiet_errors():
      ds = gdal.Open(self.getTestFilePath('uint16_lossless.jpg'))
      self.assertIsNone(ds)

  def test_jpeg_copy_mdd(self):
    src_ds = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER).Create('', 1, 1)
    src_ds.SetMetadataItem('FOO', 'BAR')
    src_ds.SetMetadataItem('BAR', 'BAZ', 'OTHER_DOMAIN')
    src_ds.SetMetadataItem('should_not', 'be_copied', 'IMAGE_STRUCTURE')

    filename = '/vsimem/test_jpeg_copy_mdd.jpg'
    with (
        gcore_util.GdalUnlinkWhenDone(filename),
        gcore_util.GdalUnlinkWhenDone(filename + '.aux.xml'),
        gdal.config_option('GDAL_PAM_ENABLED', 'YES'),
    ):
      out_ds = self.driver.CreateCopy(filename, src_ds)
      del out_ds
      with gdal.Open(filename) as ds:
        self.assertEqual(
            set(ds.GetMetadataDomainList()), {'', 'DERIVED_SUBDATASETS'}
        )
        self.assertEqual(ds.GetMetadata_Dict(), {'FOO': 'BAR'})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {})

      out_ds = self.driver.CreateCopy(
          filename, src_ds, options=['COPY_SRC_MDD=NO']
      )
      del out_ds
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetMetadata_Dict(), {})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {})

      out_ds = self.driver.CreateCopy(
          filename, src_ds, options=['COPY_SRC_MDD=YES']
      )
      del out_ds
      with gdal.Open(filename) as ds:
        self.assertEqual(
            set(ds.GetMetadataDomainList()),
            {'', 'DERIVED_SUBDATASETS', 'OTHER_DOMAIN'},
        )
        self.assertEqual(ds.GetMetadata_Dict(), {'FOO': 'BAR'})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {'BAR': 'BAZ'})

      out_ds = self.driver.CreateCopy(
          filename, src_ds, options=['SRC_MDD=OTHER_DOMAIN']
      )
      del out_ds
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetMetadata_Dict(), {})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {'BAR': 'BAZ'})

      out_ds = self.driver.CreateCopy(
          filename, src_ds, options=['SRC_MDD=', 'SRC_MDD=OTHER_DOMAIN']
      )
      del out_ds
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetMetadata_Dict(), {'FOO': 'BAR'})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {'BAR': 'BAZ'})

  def test_jpeg_read_DNG_tags(self):
    with gdal.Open(
        self.getTestFilePath(
            'DNG_CameraSerialNumber_and_DNG_UniqueCameraModel.jpg'
        )
    ) as ds:
      self.assertEqual(
          ds.GetMetadataItem('DNG_CameraSerialNumber'), 'SerialNumber'
      )
      self.assertEqual(
          ds.GetMetadataItem('DNG_UniqueCameraModel'), 'CameraModel'
      )

  def test_jpeg_read_DNG_tags_same_value_as_EXIF(self):
    with gdal.Open(self.getTestFilePath('DNG_and_EXIF_same_values.jpg')) as ds:
      self.assertIsNone(ds.GetMetadataItem('DNG_CameraSerialNumber'))
      self.assertIsNone(ds.GetMetadataItem('DNG_UniqueCameraModel'))
      self.assertEqual(
          ds.GetMetadataItem('EXIF_BodySerialNumber'), 'SerialNumber'
      )
      self.assertEqual(ds.GetMetadataItem('EXIF_Model'), 'CameraModel')

  def test_jpeg_read_pix4d_xmp_crs_vertcs_orthometric(self):
    with gdal.Open(
        self.getTestFilePath('pix4d_xmp_crs_vertcs_orthometric.jpg')
    ) as ds:
      self.assertEqual(
          ds.GetMetadataDomainList(), ['xml:XMP', 'DERIVED_SUBDATASETS']
      )
      srs = ds.GetSpatialRef()
      self.assertIsNotNone(srs)
      self.assertEqual(srs.GetAuthorityCode('GEOGCS'), '6318')
      self.assertEqual(srs.GetAuthorityCode('VERT_CS'), '6360')

  def test_jpeg_read_pix4d_xmp_crs_vertcs_ellipsoidal(self):
    with gdal.Open(
        self.getTestFilePath('pix4d_xmp_crs_vertcs_ellipsoidal.jpg')
    ) as ds:
      srs = ds.GetSpatialRef()
      self.assertIsNotNone(srs)
      self.assertEqual(srs.GetAuthorityCode(None), '6319')

  def test_jpeg_create_copy_only_visible_at_close_time(self):
    out_filename = os.path.join(self.create_tempdir().full_path, 'tmp.jpg')
    observations: list[tuple[float, gdal.StatBuf | None]] = []

    def record_progress(pct: float, msg: str, user_data: object) -> bool:
      del msg, user_data
      observations.append((pct, gdal.VSIStatL(out_filename)))
      return True

    self.assertEqual(
        self.driver.GetMetadataItem(
            gdal.DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME
        ),
        'YES',
    )
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
      out_ds = self.driver.CreateCopy(
          out_filename,
          src_ds,
          options=['@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME=YES'],
          callback=record_progress,
      )
      del out_ds

    before_close = [stat for pct, stat in observations if pct < 1.0]
    self.assertNotEmpty(before_close)
    for stat in before_close:
      self.assertIsNone(stat)

    with gdal.Open(out_filename) as ds:
      self.assertGreater(ds.GetRasterBand(1).Checksum(), 0)


if __name__ == '__main__':
  googletest.main()
