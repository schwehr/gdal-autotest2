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
###############################################################################
# Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################
"""Tests for the MEM raster driver in GDAL.

Format is described here:

https://gdal.org/drivers/raster/mem.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/mem.py
"""

import ctypes
import struct

from osgeo import gdal
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.MEM_DRIVER
EXT = 'No extension'

_LL_CRS = (
    'GEOGCS[\\"WGS 84\\",DATUM[\\"WGS_1984\\",SPHEROID[\\"WGS 84\\",'
    '6378137,298.257223563,AUTHORITY[\\"EPSG\\",\\"7030\\"]],'
    'AUTHORITY[\\"EPSG\\",\\"6326\\"]],PRIMEM[\\"Greenwich\\",0,'
    'AUTHORITY[\\"EPSG\\",\\"8901\\"]],UNIT[\\"degree\\",0.0174532925199433,'
    'AUTHORITY[\\"EPSG\\",\\"9122\\"]],AXIS[\\"Latitude\\",NORTH],'
    'AXIS[\\"Longitude\\",EAST],AUTHORITY[\\"EPSG\\",\\"4326\\"]]'
)


def _user_srs_to_wkt(user_text: str) -> str:
  srs = osr.SpatialReference()
  srs.SetFromUserInput(user_text)
  return srs.ExportToWkt()


class MemTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    gdal.UseExceptions()

  def testDriver(self):
    self.CheckDriver()

  def test_mem_1(self):
    with self.driver.Create('mem_1.mem', 50, 3) as ds:
      self.assertEqual(ds.GetProjection(), '')
      self.assertIsNone(ds.GetGeoTransform(can_return_null=True))

      raw_data = b''.join(struct.pack('f', v) for v in range(150))
      ds.WriteRaster(
          0, 0, 50, 3, raw_data, buf_type=gdal.GDT_Float32, band_list=[1]
      )

      wkt = _user_srs_to_wkt('EPSG:26711')
      ds.SetProjection(wkt)

      gt = (440720, 5, 0, 3751320, 0, -5)
      ds.SetGeoTransform(gt)

      band = ds.GetRasterBand(1)
      band.SetNoDataValue(-1.0)

      wkt_gcp = _user_srs_to_wkt('EPSG:4326')
      gcps = [gdal.GCP(0, 1, 2, 3, 4)]
      ds.SetGCPs([], '')
      ds.SetGCPs(gcps, wkt_gcp)
      ds.SetGCPs([], '')
      ds.SetGCPs(gcps, wkt_gcp)
      ds.SetGCPs(gcps, wkt_gcp)

      self.assertEqual(band.GetNoDataValue(), -1.0)
      self.assertEqual(ds.GetProjection(), wkt)
      self.assertEqual(ds.GetGeoTransform(), gt)
      self.assertEqual(band.Checksum(), 1531)
      self.assertEqual(ds.GetGCPCount(), 1)
      self.assertLen(ds.GetGCPs(), 1)
      self.assertEqual(ds.GetGCPProjection(), wkt_gcp)

      self.assertEqual(band.DeleteNoDataValue(), gdal.CE_None)
      self.assertIsNone(band.GetNoDataValue())

  def test_mem_open_missing_fields(self):
    with self.assertRaisesRegex(
        RuntimeError,
        'Opening a MEM dataset with the MEM:::DATAPOINTER= syntax is no '
        'longer supported by default for security reasons',
    ):
      gdal.Open('MEM:::')

    with gdal.config_option('GDAL_MEM_ENABLE_OPEN', 'YES'):
      with self.assertRaisesRegex(RuntimeError, 'Missing required field'):
        gdal.Open('MEM:::')

  @parameterized.named_parameters(
      dict(
          testcase_name='explicit_offsets',
          dsname_template=(
              'MEM:::DATAPOINTER=0x{ptr:X},PIXELS={width},LINES={height},'
              'BANDS=1,DATATYPE=Float32,PIXELOFFSET=4,'
              'LINEOFFSET={line_offset},BANDOFFSET=0'
          ),
      ),
      dict(
          testcase_name='default_offsets',
          dsname_template=(
              'MEM:::DATAPOINTER=0x{ptr:X},PIXELS={width},LINES={height},'
              'DATATYPE=Float32'
          ),
      ),
  )
  def test_mem_2(self, dsname_template):
    width = 50
    height = 3
    float_buf = (ctypes.c_float * (width * height))()
    ptr = ctypes.addressof(float_buf)
    dsname = dsname_template.format(
        ptr=ptr, width=width, height=height, line_offset=width * 4
    )

    for i in range(width * height):
      float_buf[i] = 5.0

    with self.assertRaisesRegex(
        RuntimeError,
        'Opening a MEM dataset with the MEM:::DATAPOINTER= syntax is no '
        'longer supported by default for security reasons',
    ):
      gdal.Open(dsname)

    with gdal.config_option('GDAL_MEM_ENABLE_OPEN', 'YES'):
      with gdal.Open(dsname) as dsro:
        self.assertIsNotNone(dsro)
        self.assertEqual(dsro.GetRasterBand(1).Checksum(), 750)

      with gdal.Open(dsname, gdal.GA_Update) as dsup:
        self.assertIsNotNone(dsup)
        dsup.GetRasterBand(1).Fill(100.0)
        dsup.FlushCache()

    self.assertEqual(float_buf[0], 100.0)

  @parameterized.named_parameters(
      dict(
          testcase_name='proj_crs',
          srs_param='+proj=laea +lon_0=147 +lat_0=-42',
          expected_sr='Lambert',
      ),
      dict(
          testcase_name='wkt_crs',
          srs_param=f'"{_LL_CRS}"',
          expected_sr='GEOGCS',
      ),
  )
  def test_geotransform(self, srs_param, expected_sr):
    float_buf = (ctypes.c_float * (50 * 3))()
    ptr = ctypes.addressof(float_buf)
    dsname = (
        f'MEM:::DATAPOINTER=0x{ptr:X},'
        'GEOTRANSFORM=-1e+06/1953.125/0/1e+06/0/-3906.25,'
        f'PIXELS=50,LINES=3,SPATIALREFERENCE={srs_param},DATATYPE=Float32'
    )
    with gdal.config_option('GDAL_MEM_ENABLE_OPEN', 'YES'):
      with gdal.Open(dsname) as dsro:
        self.assertIsNotNone(dsro)
        self.assertEqual(
            dsro.GetGeoTransform(), (-1e06, 1953.125, 0, 1e06, 0, -3906.25)
        )
        self.assertIn(expected_sr, dsro.GetProjectionRef())

  def test_geotransform_invalid_srs(self):
    float_buf = (ctypes.c_float * (50 * 3))()
    ptr = ctypes.addressof(float_buf)
    dsname = (
        f'MEM:::DATAPOINTER=0x{ptr:X},'
        'GEOTRANSFORM=-1e+06/1953.125/0/1e+06/0/-3906.25,'
        'PIXELS=50,LINES=3,SPATIALREFERENCE=bogus,DATATYPE=Float32'
    )
    with gdal.config_option('GDAL_MEM_ENABLE_OPEN', 'YES'):
      with gdal.quiet_errors():
        with gdal.Open(dsname) as dsro:
          self.assertIsNotNone(dsro)
          self.assertEqual(
              dsro.GetGeoTransform(), (-1e06, 1953.125, 0, 1e06, 0, -3906.25)
          )
          self.assertEqual(dsro.GetProjectionRef(), '')

  def test_mem_3(self):
    with self.driver.Create('MEM:::', 1, 1, 1) as ds:
      self.assertIsNotNone(ds)

  def test_mem_4(self):
    ds = self.driver.Create('', 100, 100, 3)
    expected_checksum = [0, 0, 0]
    for i in range(3):
      checksum = ds.GetRasterBand(i + 1).Checksum()
      self.assertEqual(checksum, expected_checksum[i])

    ds.GetRasterBand(1).Fill(255)
    expected_checksum = [57182, 0, 0]
    for i in range(3):
      checksum = ds.GetRasterBand(i + 1).Checksum()
      self.assertEqual(checksum, expected_checksum[i])

    self.assertEqual(
        ds.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'), 'BAND'
    )

  def test_mem_5(self):
    with self.driver.Create(
        '', 100, 100, 3, options=['INTERLEAVE=PIXEL']
    ) as ds:
      expected_checksum = [0, 0, 0]
      for i in range(3):
        self.assertEqual(
            ds.GetRasterBand(i + 1).Checksum(), expected_checksum[i]
        )

      ds.GetRasterBand(1).Fill(255)
      expected_checksum = [57182, 0, 0]
      for i in range(3):
        self.assertEqual(
            ds.GetRasterBand(i + 1).Checksum(), expected_checksum[i]
        )

      self.assertEqual(
          ds.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'), 'PIXEL'
      )

  def test_mem_6(self):
    with self.assertRaisesRegex(RuntimeError, 'Multiplication overflow'):
      self.driver.Create('', 1, 1, 0x7FFFFFFF, gdal.GDT_Float64)

    with self.assertRaisesRegex(RuntimeError, 'Multiplication overflow'):
      self.driver.Create('', 0x7FFFFFFF, 0x7FFFFFFF, 16)

    with self.assertRaisesRegex(RuntimeError, 'Multiplication overflow'):
      self.driver.Create('', 0x7FFFFFFF, 0x7FFFFFFF, 1, gdal.GDT_Float64)

    with self.assertRaisesRegex(RuntimeError, 'cannot allocate'):
      self.driver.Create(
          '', 0x7FFFFFFF, 0x7FFFFFFF, 1, options=['INTERLEAVE=PIXEL']
      )

    with self.assertRaisesRegex(RuntimeError, 'cannot allocate'):
      self.driver.Create('', 0x7FFFFFFF, 0x7FFFFFFF, 1)

    # Skip a 32-bit system test here.

    with self.driver.Create('', 0x7FFFFFFF, 0x7FFFFFFF, 0) as ds:
      with self.assertRaisesRegex(RuntimeError, 'cannot allocate'):
        ds.AddBand(gdal.GDT_Float64)

  def test_mem_7(self):
    with self.driver.Create('MEM:::', 1, 1, 1) as ds:
      self.assertEqual(ds.AddBand(gdal.GDT_Byte, []), gdal.CE_None)
      self.assertEqual(ds.RasterCount, 2)

  def test_mem_8(self):
    with self.driver.Create('MEM:::', 1, 1, 1) as ds:
      band = ds.GetRasterBand(1)
      band.SetDefaultHistogram(0, 255, [])
      band.SetDefaultHistogram(1, 2, [5, 6])
      band.SetDefaultHistogram(1, 2, [3000000000, 4])
      hist = band.GetDefaultHistogram(force=0)

    self.assertEqual(hist, (1.0, 2.0, 2, [3000000000, 4]))

  @parameterized.parameters('BAND', 'PIXEL')
  def test_mem_9(self, interleave):
    src_path = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    with gdal.Open(src_path) as src_ds:
      with self.driver.CreateCopy(
          '', src_ds, options=[f'INTERLEAVE={interleave}']
      ) as out_ds:
        ref_data = src_ds.GetRasterBand(2).ReadRaster(20, 8, 4, 5)
        got_data = out_ds.GetRasterBand(2).ReadRaster(20, 8, 4, 5)
        self.assertEqual(got_data, ref_data)

        ref_data = src_ds.GetRasterBand(2).ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100
        )
        got_data = out_ds.GetRasterBand(2).ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100
        )
        self.assertEqual(got_data, ref_data)

        ref_data = src_ds.ReadRaster(20, 8, 4, 5)
        got_data = out_ds.ReadRaster(20, 8, 4, 5)
        self.assertEqual(got_data, ref_data)

        ref_data = src_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_band_space=1
        )
        got_data = out_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_band_space=1
        )
        self.assertEqual(got_data, ref_data)

        out_ds.WriteRaster(
            20, 8, 4, 5, got_data, buf_pixel_space=3, buf_band_space=1
        )
        got_data = out_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_band_space=1
        )
        self.assertEqual(got_data, ref_data)

        ref_data = src_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100, buf_band_space=1
        )
        got_data = out_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100, buf_band_space=1
        )
        self.assertEqual(got_data, ref_data)

        ref_data = src_ds.ReadRaster(
            20,
            20,
            4,
            5,
            buf_type=gdal.GDT_Int32,
            buf_pixel_space=12,
            buf_band_space=4,
        )
        got_data = out_ds.ReadRaster(
            20,
            20,
            4,
            5,
            buf_type=gdal.GDT_Int32,
            buf_pixel_space=12,
            buf_band_space=4,
        )
        self.assertEqual(got_data, ref_data)

        out_ds.WriteRaster(
            20,
            20,
            4,
            5,
            got_data,
            buf_type=gdal.GDT_Int32,
            buf_pixel_space=12,
            buf_band_space=4,
        )
        got_data = out_ds.ReadRaster(
            20,
            20,
            4,
            5,
            buf_type=gdal.GDT_Int32,
            buf_pixel_space=12,
            buf_band_space=4,
        )
        self.assertEqual(got_data, ref_data)

        # Test IReadBlock.
        ref_data = src_ds.GetRasterBand(1).ReadRaster(
            0, 10, src_ds.RasterXSize, 1
        )
        out_ds.FlushCache()
        got_data = out_ds.GetRasterBand(1).ReadBlock(0, 10)
        self.assertEqual(got_data, ref_data)

        # Test IRasterIO(GF_Write, ...).
        ref_data = src_ds.GetRasterBand(1).ReadRaster(2, 3, 4, 5)
        out_ds.GetRasterBand(1).WriteRaster(6, 7, 4, 5, ref_data)
        got_data = out_ds.GetRasterBand(1).ReadRaster(6, 7, 4, 5)
        self.assertEqual(got_data, ref_data)

        # Test IRasterIO(GF_Write, change data type) + IWriteBlock() +
        # IRasterIO(GF_Read, change data type).
        ref_data = src_ds.GetRasterBand(1).ReadRaster(
            10, 11, 4, 5, buf_type=gdal.GDT_Int32
        )
        out_ds.GetRasterBand(1).WriteRaster(
            10, 11, 4, 5, ref_data, buf_type=gdal.GDT_Int32
        )
        got_data = out_ds.GetRasterBand(1).ReadRaster(
            10, 11, 4, 5, buf_type=gdal.GDT_Int32
        )
        self.assertEqual(got_data, ref_data)

        ref_data = src_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5)
        got_data = out_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5)
        self.assertEqual(got_data, ref_data)

        # Test IRasterIO(GF_Write, resampling) + IWriteBlock() +
        # IRasterIO(GF_Read, resampling).
        ref_data = src_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5)
        ref_data_zoomed = src_ds.GetRasterBand(1).ReadRaster(
            10, 11, 4, 5, 8, 10
        )
        out_ds.GetRasterBand(1).WriteRaster(10, 11, 8, 10, ref_data, 4, 5)
        got_data = out_ds.GetRasterBand(1).ReadRaster(10, 11, 8, 10)
        self.assertEqual(got_data, ref_data_zoomed)

        got_data = out_ds.GetRasterBand(1).ReadRaster(10, 11, 8, 10, 4, 5)
        self.assertEqual(got_data, ref_data)

  @parameterized.parameters('BAND', 'PIXEL')
  def test_mem_9_full_width_write(self, interleave):
    src_path = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    with gdal.Open(src_path) as src_ds:
      with self.driver.CreateCopy(
          '', src_ds, options=[f'INTERLEAVE={interleave}']
      ) as out_ds:
        for i in range(3):
          out_ds.GetRasterBand(i + 1).Fill(0)
        ref_data = src_ds.ReadRaster(
            0, 10, out_ds.RasterXSize, 5, buf_pixel_space=3, buf_band_space=1
        )
        out_ds.WriteRaster(
            0,
            10,
            out_ds.RasterXSize,
            5,
            ref_data,
            buf_pixel_space=3,
            buf_band_space=1,
        )
        got_data = out_ds.ReadRaster(
            0, 10, out_ds.RasterXSize, 5, buf_pixel_space=3, buf_band_space=1
        )
        self.assertEqual(got_data, ref_data)

  @parameterized.parameters('BAND', 'PIXEL')
  def test_mem_9_subwindow_write(self, interleave):
    src_path = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    with gdal.Open(src_path) as src_ds:
      with self.driver.CreateCopy(
          '', src_ds, options=[f'INTERLEAVE={interleave}']
      ) as out_ds:
        for i in range(3):
          out_ds.GetRasterBand(i + 1).Fill(0)
        ref_data = src_ds.ReadRaster(
            4, 10, 15, 5, buf_pixel_space=3, buf_band_space=1
        )
        out_ds.WriteRaster(
            4, 10, 15, 5, ref_data, buf_pixel_space=3, buf_band_space=1
        )
        got_data = out_ds.ReadRaster(
            4, 10, 15, 5, buf_pixel_space=3, buf_band_space=1
        )
        self.assertEqual(got_data, ref_data)

  def test_mem_10(self):
    # Error case: building overview on a 0 band dataset.
    with self.driver.Create('', 1, 1, 0) as ds:
      with self.assertRaisesRegex(RuntimeError, 'Dataset has zero bands'):
        ds.BuildOverviews('NEAR', [2])

    # Requesting overviews when there are none.
    with self.driver.Create('', 1, 1) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)
      self.assertIsNone(ds.GetRasterBand(1).GetOverview(-1))
      self.assertIsNone(ds.GetRasterBand(1).GetOverview(0))

    # Single band case.
    byte_path = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    with gdal.Open(byte_path) as byte_ds:
      with self.driver.CreateCopy('', byte_ds) as ds:
        for _ in range(2):
          self.assertEqual(ds.BuildOverviews('NEAR', [2]), gdal.CE_None)
          self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
          self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)

        self.assertEqual(ds.BuildOverviews('NEAR', [4]), gdal.CE_None)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 2)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(1).Checksum(), 328)

        self.assertEqual(ds.BuildOverviews('NEAR', [2, 4]), gdal.CE_None)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 2)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(1).Checksum(), 328)

        # Average in one or several steps gives the same result.
        ds.GetRasterBand(1).GetOverview(0).Fill(0)
        ds.GetRasterBand(1).GetOverview(1).Fill(0)

        self.assertEqual(ds.BuildOverviews('AVERAGE', [2, 4]), gdal.CE_None)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 2)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1152)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(1).Checksum(), 240)

        ds.GetRasterBand(1).GetOverview(0).Fill(0)
        ds.GetRasterBand(1).GetOverview(1).Fill(0)

        self.assertEqual(ds.BuildOverviews('AVERAGE', [2]), gdal.CE_None)
        self.assertEqual(ds.BuildOverviews('AVERAGE', [4]), gdal.CE_None)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 2)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1152)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(1).Checksum(), 240)

      # Clean overviews.
      with self.driver.CreateCopy('', byte_ds) as ds:
        self.assertEqual(ds.BuildOverviews('NEAR', [2]), gdal.CE_None)
        self.assertEqual(ds.BuildOverviews('NONE', []), gdal.CE_None)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)

    # Multiple band case.
    rgb_path = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    with gdal.Open(rgb_path) as rgb_ds:
      with self.driver.CreateCopy('', rgb_ds) as ds:
        self.assertEqual(ds.BuildOverviews('NEAR', [2]), gdal.CE_None)
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 5057)
        self.assertEqual(ds.GetRasterBand(2).GetOverview(0).Checksum(), 5304)
        self.assertEqual(ds.GetRasterBand(3).GetOverview(0).Checksum(), 5304)

  def test_mem_11(self):
    # Error case: creating mask band on a 0 band dataset.
    with self.driver.Create('', 1, 1, 0) as ds:
      self.assertNotEqual(ds.CreateMaskBand(gdal.GMF_PER_DATASET), gdal.CE_None)

    # Per dataset mask on single band dataset.
    with self.driver.Create('', 1, 1) as ds:
      self.assertEqual(ds.CreateMaskBand(gdal.GMF_PER_DATASET), gdal.CE_None)
      band = ds.GetRasterBand(1)
      self.assertEqual(band.GetMaskFlags(), gdal.GMF_PER_DATASET)
      self.assertFalse(band.IsMaskBand())
      mask = band.GetMaskBand()
      self.assertTrue(mask.IsMaskBand())
      self.assertEqual(mask.Checksum(), 0)
      mask.Fill(255)
      self.assertEqual(mask.Checksum(), 3)

    # Per dataset mask is shared by all bands.
    with self.driver.Create('', 1, 1, 2) as ds:
      self.assertEqual(ds.CreateMaskBand(gdal.GMF_PER_DATASET), gdal.CE_None)
      mask1 = ds.GetRasterBand(1).GetMaskBand()
      mask1.Fill(255)
      mask2 = ds.GetRasterBand(2).GetMaskBand()
      self.assertEqual(mask2.Checksum(), 3)

    # Same, called on band 2.
    with self.driver.Create('', 1, 1, 2) as ds:
      self.assertEqual(
          ds.GetRasterBand(2).CreateMaskBand(gdal.GMF_PER_DATASET), gdal.CE_None
      )
      mask2 = ds.GetRasterBand(2).GetMaskBand()
      mask2.Fill(255)
      mask1 = ds.GetRasterBand(1).GetMaskBand()
      self.assertEqual(mask1.Checksum(), 3)

    # Per band masks.
    with self.driver.Create('', 1, 1, 2) as ds:
      self.assertEqual(ds.GetRasterBand(1).CreateMaskBand(0), gdal.CE_None)
      self.assertEqual(ds.GetRasterBand(2).CreateMaskBand(0), gdal.CE_None)
      mask1 = ds.GetRasterBand(1).GetMaskBand()
      mask2 = ds.GetRasterBand(2).GetMaskBand()
      mask2.Fill(255)
      self.assertEqual(mask1.Checksum(), 0)
      self.assertEqual(mask2.Checksum(), 3)

  def test_mem_12(self):
    # Per-band mask with overviews.
    with self.driver.Create('', 10, 10, 2) as ds:
      ds.GetRasterBand(1).CreateMaskBand(0)
      ds.GetRasterBand(1).GetMaskBand().Fill(127)
      ds.BuildOverviews('NEAR', [2])
      self.assertEqual(
          ds.GetRasterBand(1).GetOverview(0).GetMaskBand().Checksum(), 267
      )
      # Default mask.
      self.assertEqual(
          ds.GetRasterBand(2).GetOverview(0).GetMaskBand().Checksum(), 283
      )

    # Per-dataset mask with overviews.
    with self.driver.Create('', 10, 10, 2) as ds:
      ds.CreateMaskBand(gdal.GMF_PER_DATASET)
      ds.GetRasterBand(1).GetMaskBand().Fill(127)
      ds.BuildOverviews('NEAR', [2])
      checksum = ds.GetRasterBand(1).GetOverview(0).GetMaskBand().Checksum()
      self.assertEqual(checksum, 267)
      self.assertEqual(
          ds.GetRasterBand(2).GetOverview(0).GetMaskBand().Checksum(), checksum
      )

  def test_mem_rat(self):
    with self.driver.Create('', 1, 1) as ds:
      band = ds.GetRasterBand(1)
      band.SetDefaultRAT(gdal.RasterAttributeTable())
      self.assertIsNotNone(band.GetDefaultRAT())
      band.SetDefaultRAT(None)
      self.assertIsNone(band.GetDefaultRAT())

  def test_mem_categorynames(self):
    with self.driver.Create('', 1, 1) as ds:
      band = ds.GetRasterBand(1)
      band.SetCategoryNames(['foo'])
      self.assertEqual(band.GetCategoryNames(), ['foo'])
      band.SetCategoryNames([])
      self.assertIsNone(band.GetCategoryNames())

  def test_mem_colortable(self):
    with self.driver.Create('', 1, 1) as ds:
      band = ds.GetRasterBand(1)
      ct = gdal.ColorTable()
      ct.SetColorEntry(0, (255, 255, 255, 255))
      band.SetColorTable(ct)
      self.assertEqual(band.GetColorTable().GetCount(), 1)
      band.SetColorTable(None)
      self.assertIsNone(band.GetColorTable())

  def test_mem_dataset_rasterio_non_nearest_resampling_source_with_ovr(self):
    with self.driver.Create('', 10, 10, 3) as ds:
      ds.GetRasterBand(1).Fill(255)
      ds.BuildOverviews('NONE', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(10)

      got_data = struct.unpack(
          'B' * 5 * 5 * 3, ds.ReadRaster(0, 0, 10, 10, 5, 5)
      )
      self.assertEqual(got_data[0], 10)

      got_data = struct.unpack(
          'B' * 5 * 5 * 3,
          ds.ReadRaster(0, 0, 10, 10, 5, 5, resample_alg=gdal.GRIORA_Cubic),
      )
      self.assertEqual(got_data[0], 10)

  def test_mem_nodata_int64(self):
    with self.driver.Create('', 1, 1, 1, gdal.GDT_Int64) as ds:
      val = -(1 << 63)
      band = ds.GetRasterBand(1)
      self.assertEqual(band.SetNoDataValue(val), gdal.CE_None)
      self.assertEqual(band.GetNoDataValue(), val)

  def test_mem_nodata_uint64(self):
    with self.driver.Create('', 1, 1, 1, gdal.GDT_UInt64) as ds:
      val = (1 << 64) - 1
      band = ds.GetRasterBand(1)
      self.assertEqual(band.SetNoDataValue(val), gdal.CE_None)
      self.assertEqual(band.GetNoDataValue(), val)

  def test_mem_alpha_ismaskband(self):
    with self.driver.Create('', 1, 1, 2) as ds:
      ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_AlphaBand)
      self.assertFalse(ds.GetRasterBand(1).IsMaskBand())
      self.assertTrue(ds.GetRasterBand(2).IsMaskBand())

  def test_mem_gdt_unknown(self):
    with self.assertRaisesRegex(
        RuntimeError, 'Illegal GDT_Unknown/GDT_TypeCount argument'
    ):
      self.driver.Create('', 1, 1, 1, gdal.GDT_Unknown)

    with self.driver.Create('', 1, 1, 0, gdal.GDT_Unknown) as ds:
      with self.assertRaisesRegex(
          RuntimeError, 'Illegal GDT_Unknown/GDT_TypeCount argument'
      ):
        ds.AddBand(gdal.GDT_Unknown)

  def test_mem_extent(self):
    with self.driver.Create('', 2, 10) as ds:
      self.assertIsNone(ds.GetExtent())
      self.assertIsNone(ds.GetExtentWGS84LongLat())

      srs = osr.SpatialReference(epsg=3857)
      ds.SetSpatialRef(srs)
      ds.SetGeoTransform([1000, 1, 0, 2000, 0, -2])

      self.assertTrue(ds.TestCapability(gdal.GDsCFastGetExtent))
      self.assertEqual(ds.GetExtent(), (1000, 1002, 1980, 2000))
      self.assertTrue(ds.TestCapability(gdal.GDsCFastGetExtentWGS84LongLat))
      self.assertIterAlmostEqual(
          ds.GetExtentWGS84LongLat(),
          (
              0.008983152841195215,
              0.009001119146877604,
              0.017786642339884726,
              0.01796630538796444,
          ),
          places=9,
      )

  def test_mem_flush_mask_band_on_close(self):
    errors = []

    def handler(err_class, err_no, msg):
      del err_no  # Unused.
      if err_class != gdal.CE_Debug:
        errors.append((err_class, msg))

    ds = self.driver.Create('', 1, 1)
    ds.CreateMaskBand(gdal.GMF_PER_DATASET)
    ds.GetRasterBand(1).GetMaskBand().Fill(255)
    gdal.PushErrorHandler(handler)
    try:
      self.assertEqual(ds.Close(), gdal.CE_None)
    finally:
      gdal.PopErrorHandler()
    self.assertEmpty(errors)


if __name__ == '__main__':
  googletest.main()
