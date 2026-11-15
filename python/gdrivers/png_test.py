# Copyright 2017 Google Inc. All Rights Reserved.
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
# This is a complete rewrite of files licensed as follows:
#
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at mines-paris.org>
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
#
# And autotest/gdrivers/png_profile.py:
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.
#
# The sRGB.icc test profile is by Marti Maria (littleCMS), zlib license, from:
# http://sourceforge.net/projects/openicc/files/OpenICC-Profiles/

"""Tests for the PNG driver.

Format is described here:

https://gdal.org/drivers/raster/png.html

Rewrite of png.py and png_profile.py
(at commit ea75dfb98cb6a6fe27d826f840178e8e6b6fe626):

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/png.py
https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/png_profile.py
"""

import array
import base64
import glob
import os
import pathlib

from osgeo import gdal

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.PNG_DRIVER
EXT = '.png'

BACKGROUND_COLOR = 'BACKGROUND_COLOR'


def _interrupt_callback(pct: float, message: str, user_data: object) -> bool:
  del message, user_data
  return pct <= 0.5


def _format_color_tuple(values: tuple[float, float, float]) -> str:
  return ','.join(str(v) for v in values)


def _parse_color_tuple(text: str) -> list[float]:
  return [float(part) for part in text.split(',')]


class PngTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self) -> None:  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    gdal.DontUseExceptions()

  def getTestFilePath(self, filename: str) -> str:
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self) -> None:
    self.CheckDriver()

  def testInfo(self) -> None:
    with gdal.quiet_errors():
      globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
      for jsonfile in sorted(glob.glob(globpath)):
        filepath = os.path.splitext(jsonfile)[0]
        with self.subTest(input=os.path.basename(filepath)):
          self.CheckOpen(filepath)
          self.CheckInfo()

  def test_png_1(self) -> None:
    self.CheckOpen(self.getTestFilePath('test.png'))
    self.CheckShape(400, 400, 1)
    self.CheckBand(1, 57921, gdal.GDT_Byte)
    assert self.src is not None
    self.assertEqual(
        sorted(os.path.basename(f) for f in self.src.GetFileList()),
        ['test.png', 'test.wld'],
    )

  def test_png_2(self) -> None:
    self.CheckOpen(self.getTestFilePath('test.png'))
    self.CheckCreateCopy(
        check_geotransform=False,
        checksums=[57921],
        vsimem=True,
        remove_result=True,
    )

  def test_png_3(self) -> None:
    with gdal.Open(self.getTestFilePath('test.png')) as ds:
      band = ds.GetRasterBand(1)
      cm = band.GetRasterColorTable()
      self.assertIsNotNone(cm)
      self.assertEqual(cm.GetCount(), 16)
      self.assertEqual(cm.GetColorEntry(0), (255, 255, 255, 0))
      self.assertEqual(cm.GetColorEntry(1), (255, 255, 208, 255))
      cm = None

      self.assertEqual(band.GetNoDataValue(), 0.0)

      gt_expected = (700000.305, 0.38, 0.01, 4287500.695, -0.01, -0.38)
      self.assertIterAlmostEqual(
          ds.GetGeoTransform(), gt_expected, delta=0.0001
      )

  def test_png_4(self) -> None:
    self.CheckOpen(
        gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif'),
        check_driver=gdrivers_util.GTIFF_DRIVER,
    )
    self.CheckCreateCopy(
        check_geotransform=False,
        check_projection=False,
        checksums=[21212, 21053, 21349],
        vsimem=True,
        remove_result=True,
    )

  def test_png_5(self) -> None:
    self.CheckOpen(self.getTestFilePath('rgba16.png'))
    self.CheckShape(20, 20, 4)
    self.CheckBand(1, 1886, gdal.GDT_UInt16)
    self.CheckBand(2, 2042, gdal.GDT_UInt16)
    self.CheckBand(3, 1815, gdal.GDT_UInt16)
    self.CheckBand(4, 4873, gdal.GDT_UInt16)

  def test_png_6(self) -> None:
    self.CheckOpen(self.getTestFilePath('rgba16.png'))
    self.CheckCreateCopy(
        checksums=[1886, 2042, 1815, 4873],
        vsimem=True,
        remove_result=True,
    )

  def test_png_7(self) -> None:
    filename = '/vsimem/test_png_7.png'
    with gcore_util.GdalUnlinkWhenDone(filename):
      with gdal.Open(self.getTestFilePath('tbbn2c16.png')) as src_ds:
        dst_ds = self.driver.CreateCopy(filename, src_ds)
        self.assertIsNotNone(dst_ds)
        dst_ds = None

      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetMetadata()['NODATA_VALUES'], '32639 32639 32639')

  def test_png_8(self) -> None:
    with gdal.Open(self.getTestFilePath('idat_broken.png')) as ds_src:
      self.assertEmpty(ds_src.GetMetadata())
      self.assertEqual(ds_src.RasterCount, 4)

      band = ds_src.GetRasterBand(1)
      self.assertIsNotNone(band)

      gdal.ErrorReset()
      with gdal.quiet_errors():
        band.ComputeBandStats()
        err = gdal.GetLastErrorNo()
      self.assertNotEqual(err, 0)

      dst_path = os.path.join(
          self.create_tempdir().full_path, 'idat_broken.png'
      )
      gdal.ErrorReset()
      with gdal.quiet_errors():
        ds_dst = self.driver.CreateCopy(dst_path, ds_src)
        err = gdal.GetLastErrorNo()
      self.assertNotEqual(err, 0)
      self.assertIsNone(ds_dst)

  def test_png_9(self) -> None:
    self.CheckOpen(
        gdrivers_util.GetTestFilePath('gtiff/byte.tif'),
        check_driver=gdrivers_util.GTIFF_DRIVER,
    )
    self.CheckCreateCopy(
        check_geotransform=False,
        check_projection=False,
        checksums=[4672],
        vsimem=True,
        remove_result=True,
    )

  def test_png_10(self) -> None:
    filename = '/vsimem/test_png_10.png'
    with gcore_util.GdalUnlinkWhenDone(filename):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        ds = self.driver.CreateCopy('/vsistdout_redirect/' + filename, src_ds)
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
        ds = None

      with gdal.Open(filename) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_png_11(self) -> None:
    filename = '/vsimem/test_png_11.png'
    with gcore_util.GdalUnlinkWhenDone(filename):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        gdal.ErrorReset()
        with gdal.quiet_errors():
          dst_ds = self.driver.CreateCopy(
              filename, src_ds, callback=_interrupt_callback
          )
        self.assertIsNone(dst_ds)
        self.assertNotEqual(gdal.GetLastErrorNo(), 0)

  def test_png_12(self) -> None:
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with gdal.Open(gcore_util.GetTestFilePath('stefan_full_rgba.png')) as ds:
      expected_cs = [
          ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)
      ]

      # Band interleaved
      data = ds.ReadRaster(0, 0, ds.RasterXSize, ds.RasterYSize)
      with mem_driver.Create(
          '', ds.RasterXSize, ds.RasterYSize, ds.RasterCount
      ) as tmp_ds:
        tmp_ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, data)
        got_cs = [
            tmp_ds.GetRasterBand(i + 1).Checksum()
            for i in range(ds.RasterCount)
        ]
        self.assertEqual(got_cs, expected_cs)

      # Pixel interleaved
      data = ds.ReadRaster(
          0,
          0,
          ds.RasterXSize,
          ds.RasterYSize,
          buf_pixel_space=ds.RasterCount,
          buf_band_space=1,
      )
      with mem_driver.Create(
          '', ds.RasterXSize, ds.RasterYSize, ds.RasterCount
      ) as tmp_ds:
        tmp_ds.WriteRaster(
            0,
            0,
            ds.RasterXSize,
            ds.RasterYSize,
            data,
            buf_pixel_space=ds.RasterCount,
            buf_band_space=1,
        )
        got_cs = [
            tmp_ds.GetRasterBand(i + 1).Checksum()
            for i in range(ds.RasterCount)
        ]
        self.assertEqual(got_cs, expected_cs)

      # Pixel interleaved with padding
      data = ds.ReadRaster(
          0,
          0,
          ds.RasterXSize,
          ds.RasterYSize,
          buf_pixel_space=5,
          buf_band_space=1,
      )
      with mem_driver.Create(
          '', ds.RasterXSize, ds.RasterYSize, ds.RasterCount
      ) as tmp_ds:
        tmp_ds.WriteRaster(
            0,
            0,
            ds.RasterXSize,
            ds.RasterYSize,
            data,
            buf_pixel_space=5,
            buf_band_space=1,
        )
        got_cs = [
            tmp_ds.GetRasterBand(i + 1).Checksum()
            for i in range(ds.RasterCount)
        ]
        self.assertEqual(got_cs, expected_cs)

  def test_png_13(self) -> None:
    filename = '/vsimem/test_png_13.png'
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with (
        gdal.config_option('GDAL_PAM_ENABLED', 'YES'),
        gcore_util.GdalUnlinkWhenDone(filename),
        gcore_util.GdalUnlinkWhenDone(filename + '.aux.xml'),
    ):
      with mem_driver.Create('', 1, 1) as src_ds:
        src_ds.SetMetadataItem('foo', 'bar')
        src_ds.SetMetadataItem('COPYRIGHT', 'copyright value')
        src_ds.SetMetadataItem(
            'DESCRIPTION', 'will be overridden by creation option'
        )
        out_ds = self.driver.CreateCopy(
            filename,
            src_ds,
            options=['WRITE_METADATA_AS_TEXT=YES', 'DESCRIPTION=my desc'],
        )
        self.assertIsNotNone(out_ds)
        expected_md = {
            'foo': 'bar',
            'Copyright': 'copyright value',
            'Description': 'my desc',
        }
        self.assertEqual(out_ds.GetMetadata(), expected_md)
        out_ds = None

      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
      with gdal.Open(filename) as reopened_ds:
        self.assertEqual(reopened_ds.GetMetadata(), expected_md)

  def test_png_14(self) -> None:
    filename = '/vsimem/test_png_14.png'
    with (
        gdal.config_option('GDAL_PAM_ENABLED', 'YES'),
        gdal.Open(gcore_util.GetTestFilePath('oddsize1bit.tif')) as src_ds,
        gcore_util.GdalUnlinkWhenDone(filename),
        gcore_util.GdalUnlinkWhenDone(filename + '.aux.xml'),
    ):
      expected_cs = src_ds.GetRasterBand(1).Checksum()

      dst_ds = self.driver.CreateCopy(filename, src_ds)
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))

      with gdal.Open(filename) as out_ds:
        cs = out_ds.GetRasterBand(1).Checksum()
        nbits = out_ds.GetRasterBand(1).GetMetadataItem(
            'NBITS', 'IMAGE_STRUCTURE'
        )
      gdal.Unlink(filename)
      self.assertEqual(cs, expected_cs)
      self.assertEqual(nbits, '1')

      # Test explicit NBITS
      dst_ds = self.driver.CreateCopy(filename, src_ds, options=['NBITS=2'])
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename) as out_ds:
        nbits = out_ds.GetRasterBand(1).GetMetadataItem(
            'NBITS', 'IMAGE_STRUCTURE'
        )
      gdal.Unlink(filename)
      self.assertEqual(nbits, '2')

      # Test (wrong) explicit NBITS
      gdal.ErrorReset()
      with gdal.quiet_errors():
        with self.driver.CreateCopy(
            filename, src_ds, options=['NBITS=7']
        ) as dst_ds:
          self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
          self.assertIsNotNone(dst_ds)

      with gdal.Open(filename) as out_ds:
        nbits = out_ds.GetRasterBand(1).GetMetadataItem(
            'NBITS', 'IMAGE_STRUCTURE'
        )
      gdal.Unlink(filename)
      self.assertIsNone(nbits)

  @parameterized.product(
      options=[
          {},
          {'GDAL_PNG_WHOLE_IMAGE_OPTIM': 'NO'},
          {'GDAL_PNG_SINGLE_BLOCK': 'NO'},
      ],
      nbands=[1, 2, 3, 4],
      size=[(7, 8), (513, 5)],
  )
  def test_png_whole_image_optim(
      self,
      options: dict[str, str],
      nbands: int,
      size: tuple[int, int],
  ) -> None:
    xsize, ysize = size
    filename = '/vsimem/test_png_whole_image_optim.png'
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with (
        gcore_util.GdalUnlinkWhenDone(filename),
        mem_driver.Create('', xsize, ysize, nbands) as src_ds,
    ):
      src_ds.WriteRaster(
          0,
          0,
          src_ds.RasterXSize,
          src_ds.RasterYSize,
          array.array('B', [i % 256 for i in range(xsize * ysize * nbands)]),
      )
      dst_ds = self.driver.CreateCopy(filename, src_ds)
      self.assertIsNotNone(dst_ds)
      dst_ds = None

      with gdal.config_options(options):
        with gdal.Open(filename) as ds:
          for i in range(nbands):
            self.assertEqual(
                ds.GetRasterBand(i + 1).ReadRaster(),
                src_ds.GetRasterBand(i + 1).ReadRaster(),
            )

        with gdal.Open(filename) as ds:
          self.assertEqual(
              ds.GetRasterBand(1).ReadRaster(),
              src_ds.GetRasterBand(1).ReadRaster(),
          )
          self.assertEqual(
              ds.GetRasterBand(1).ReadRaster(),
              src_ds.GetRasterBand(1).ReadRaster(),
          )

        if nbands >= 2:
          with gdal.Open(filename) as ds:
            self.assertEqual(
                ds.GetRasterBand(2).ReadRaster(),
                src_ds.GetRasterBand(2).ReadRaster(),
            )
            self.assertEqual(
                ds.GetRasterBand(1).ReadRaster(),
                src_ds.GetRasterBand(1).ReadRaster(),
            )

        with gdal.Open(filename) as ds:
          blockxsize, blockysize = ds.GetRasterBand(1).GetBlockSize()
          self.assertEqual(
              ds.GetRasterBand(1).ReadBlock(0, 0),
              src_ds.GetRasterBand(1).ReadRaster(0, 0, blockxsize, blockysize),
          )

        with gdal.Open(filename) as ds:
          self.assertEqual(
              ds.GetRasterBand(1).ReadRaster(2, 3, 4, 2),
              src_ds.GetRasterBand(1).ReadRaster(2, 3, 4, 2),
          )

        with gdal.Open(filename) as ds:
          self.assertEqual(ds.ReadRaster(), src_ds.ReadRaster())

        with gdal.Open(filename) as ds:
          self.assertEqual(
              ds.ReadRaster(2, 3, 4, 2), src_ds.ReadRaster(2, 3, 4, 2)
          )

        with gdal.Open(filename) as ds:
          self.assertEqual(
              ds.GetRasterBand(1).ReadRaster(buf_type=gdal.GDT_UInt16),
              src_ds.GetRasterBand(1).ReadRaster(buf_type=gdal.GDT_UInt16),
          )

        with gdal.Open(filename) as ds:
          self.assertEqual(
              ds.ReadRaster(buf_type=gdal.GDT_UInt16),
              src_ds.ReadRaster(buf_type=gdal.GDT_UInt16),
          )

  def test_png_background_color_gray(self) -> None:
    filename = '/vsimem/png_background_color_gray.png'
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    src_ds = mem_driver.Create('', 1, 1)
    color = '123'
    src_ds.SetMetadataItem(BACKGROUND_COLOR, color)

    with gcore_util.GdalUnlinkWhenDone(filename):
      self.driver.CreateCopy(filename, src_ds)
      gdal.Unlink(filename + '.aux.xml')

      with gdal.Open(filename) as ds:
        self.assertEqual(color, ds.GetMetadataItem(BACKGROUND_COLOR))

  def test_png_background_color_gray_alpha(self) -> None:
    filename = '/vsimem/png_background_color_gray_alpha.png'
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    src_ds = mem_driver.Create('', 1, 1, 2)

    color = '123'
    src_ds.SetMetadataItem(BACKGROUND_COLOR, color)

    with gcore_util.GdalUnlinkWhenDone(filename):
      self.driver.CreateCopy(filename, src_ds)
      gdal.Unlink(filename + '.aux.xml')

      with gdal.Open(filename) as ds:
        self.assertEqual(color, ds.GetMetadataItem(BACKGROUND_COLOR))

  def test_png_background_color_index(self) -> None:
    filename = '/vsimem/png_background_color_index.png'

    ct_ds = gdal.Open(self.getTestFilePath('test.png'))
    ct = ct_ds.GetRasterBand(1).GetRasterColorTable()

    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    src_ds = mem_driver.Create('', 1, 1)
    src_ds.GetRasterBand(1).SetColorTable(ct)
    color = '3'
    src_ds.SetMetadataItem(BACKGROUND_COLOR, color)

    with gcore_util.GdalUnlinkWhenDone(filename):
      self.driver.CreateCopy(filename, src_ds)
      gdal.Unlink(filename + '.aux.xml')

      with gdal.Open(filename) as ds:
        self.assertEqual(color, ds.GetMetadataItem(BACKGROUND_COLOR))

  def test_png_background_color_rgb(self) -> None:
    filename = '/vsimem/png_background_color_rgb.png'

    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    src_ds = mem_driver.Create('', 1, 1, 3)
    color = '123,234,67'
    src_ds.SetMetadataItem(BACKGROUND_COLOR, color)

    with gcore_util.GdalUnlinkWhenDone(filename):
      self.driver.CreateCopy(filename, src_ds)
      gdal.Unlink(filename + '.aux.xml')

      with gdal.Open(filename) as ds:
        self.assertEqual(color, ds.GetMetadataItem(BACKGROUND_COLOR))

  def test_png_background_color_rgba(self) -> None:
    filename = '/vsimem/png_background_color_rgba.png'

    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    src_ds = mem_driver.Create('', 1, 1, 4)
    color = '124,235,68'
    src_ds.SetMetadataItem(BACKGROUND_COLOR, color)

    with gcore_util.GdalUnlinkWhenDone(filename):
      self.driver.CreateCopy(filename, src_ds)
      gdal.Unlink(filename + '.aux.xml')

      with gdal.Open(filename) as ds:
        self.assertEqual(color, ds.GetMetadataItem(BACKGROUND_COLOR))

  def test_png_copy_mdd(self) -> None:
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with (
        gdal.config_option('GDAL_PAM_ENABLED', 'YES'),
        gcore_util.VsimemCleanupDir('/vsimem/test_png_copy_mdd') as test_dir,
        mem_driver.Create('', 1, 1) as src_ds,
    ):
      src_ds.SetMetadataItem('FOO', 'BAR')
      src_ds.SetMetadataItem('BAR', 'BAZ', 'OTHER_DOMAIN')
      src_ds.SetMetadataItem('should_not', 'be_copied', 'IMAGE_STRUCTURE')

      filename1 = f'{test_dir}/1.png'
      dst_ds = self.driver.CreateCopy(filename1, src_ds)
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename1) as ds:
        self.assertEqual(
            set(ds.GetMetadataDomainList()), {'', 'DERIVED_SUBDATASETS'}
        )
        self.assertEqual(ds.GetMetadata_Dict(), {'FOO': 'BAR'})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {})

      filename2 = f'{test_dir}/2.png'
      dst_ds = self.driver.CreateCopy(
          filename2, src_ds, options=['COPY_SRC_MDD=NO']
      )
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename2) as ds:
        self.assertEqual(ds.GetMetadata_Dict(), {})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {})

      filename3 = f'{test_dir}/3.png'
      dst_ds = self.driver.CreateCopy(
          filename3, src_ds, options=['COPY_SRC_MDD=YES']
      )
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename3) as ds:
        self.assertEqual(
            set(ds.GetMetadataDomainList()),
            {'', 'DERIVED_SUBDATASETS', 'OTHER_DOMAIN'},
        )
        self.assertEqual(ds.GetMetadata_Dict(), {'FOO': 'BAR'})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {'BAR': 'BAZ'})

      filename4 = f'{test_dir}/4.png'
      dst_ds = self.driver.CreateCopy(
          filename4, src_ds, options=['SRC_MDD=OTHER_DOMAIN']
      )
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename4) as ds:
        self.assertEqual(ds.GetMetadata_Dict(), {})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {'BAR': 'BAZ'})

      filename5 = f'{test_dir}/5.png'
      dst_ds = self.driver.CreateCopy(
          filename5, src_ds, options=['SRC_MDD=', 'SRC_MDD=OTHER_DOMAIN']
      )
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename5) as ds:
        self.assertEqual(ds.GetMetadata_Dict(), {'FOO': 'BAR'})
        self.assertEqual(ds.GetMetadata_Dict('OTHER_DOMAIN'), {'BAR': 'BAZ'})

  def test_png_read_interlace_16_bit(self) -> None:
    with gdal.Open(self.getTestFilePath('uint16_interlaced.png')) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.DataType, gdal.GDT_UInt16)
      self.assertEqual(band.Checksum(), 4672)

  def test_png_create_copy_only_visible_at_close_time(self) -> None:
    self.assertEqual(
        self.driver.GetMetadataItem(
            gdal.DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME
        ),
        'YES',
    )
    out_filename = os.path.join(self.create_tempdir().full_path, 'tmp.png')
    stats_before_close: list[object] = []

    def callback(pct: float, message: str, user_data: object) -> bool:
      del message, user_data
      if pct < 1.0:
        stats_before_close.append(gdal.VSIStatL(out_filename))
      return True

    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
      dst_ds = self.driver.CreateCopy(
          out_filename,
          src_ds,
          options=['@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME=YES'],
          callback=callback,
      )
      self.assertIsNotNone(dst_ds)
      dst_ds = None

    self.assertNotEmpty(stats_before_close)
    self.assertEqual(stats_before_close, [None] * len(stats_before_close))

    with gdal.Open(out_filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  @parameterized.parameters(0, 9)
  def test_png_create_zlevel_valid(self, zlevel: int) -> None:
    filename = '/vsimem/test_png_zlevel_valid.png'
    with (
        gdal.ExceptionMgr(useExceptions=True),
        gcore_util.GdalUnlinkWhenDone(filename),
        gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds,
    ):
      dst_ds = self.driver.CreateCopy(
          filename, src_ds, options=[f'ZLEVEL={zlevel}']
      )
      self.assertIsNotNone(dst_ds)
      dst_ds = None
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  @parameterized.parameters(-1, 10)
  def test_png_create_zlevel_invalid(self, zlevel: int) -> None:
    filename = '/vsimem/test_png_zlevel_invalid.png'
    with (
        gdal.ExceptionMgr(useExceptions=True),
        gcore_util.GdalUnlinkWhenDone(filename),
        gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds,
    ):
      with self.assertRaisesRegex(RuntimeError, 'Illegal ZLEVEL value'):
        self.driver.CreateCopy(filename, src_ds, options=[f'ZLEVEL={zlevel}'])

  def test_png_close(self) -> None:
    out_filename = os.path.join(self.create_tempdir().full_path, 'out.png')
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
      ds = self.driver.CreateCopy(out_filename, src_ds)
      self.assertIsNotNone(ds)
      self.assertEqual(ds.Close(), gdal.CE_None)
    os.remove(out_filename)

  def _load_srgb_icc_base64(self) -> str:
    data = pathlib.Path(
        gdrivers_util.GetTestFilePath('jpegxl/sRGB.icc')
    ).read_bytes()
    return base64.b64encode(data).decode('ascii')

  def test_png_copy_icc(self) -> None:
    icc = self._load_srgb_icc_base64()
    tmpdir = self.create_tempdir().full_path
    tiff_path = os.path.join(tmpdir, 'icc_test.tif')
    png_path = os.path.join(tmpdir, 'icc_test.png')
    aux_path = png_path + '.aux.xml'

    driver_tiff = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
      ds = driver_tiff.Create(
          tiff_path, 64, 64, 3, gdal.GDT_Byte, ['SOURCE_ICC_PROFILE=' + icc]
      )
      ds2 = self.driver.CreateCopy(png_path, ds)
      self.assertIsNotNone(ds2)
      md = ds2.GetMetadata('COLOR_PROFILE')
      ds = None
      ds2 = None

      self.assertEqual(md['SOURCE_ICC_PROFILE'], icc)
      self.assertFalse(os.path.exists(aux_path))

      ds2 = gdal.Open(png_path)
      md = ds2.GetMetadata('COLOR_PROFILE')
      ds2 = None
      self.assertFalse(os.path.exists(aux_path))
      self.assertEqual(md['SOURCE_ICC_PROFILE'], icc)

      ds2 = gdal.Open(png_path)
      source_icc_profile = ds2.GetMetadataItem(
          'SOURCE_ICC_PROFILE', 'COLOR_PROFILE'
      )
      ds2 = None
      self.assertFalse(os.path.exists(aux_path))
      self.assertEqual(source_icc_profile, icc)

      driver_tiff.Delete(tiff_path)
      self.driver.Delete(png_path)

  def test_png_copy_options_icc(self) -> None:
    icc = self._load_srgb_icc_base64()
    driver_tiff = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/test_png_copy_options_icc') as d:
      tiff_path = f'{d}/icc_test.tif'
      png_path = f'{d}/icc_test.png'

      ds = driver_tiff.Create(tiff_path, 64, 64, 3, gdal.GDT_Byte)
      ds2 = self.driver.CreateCopy(
          png_path, ds, options=['SOURCE_ICC_PROFILE=' + icc]
      )
      self.assertIsNotNone(ds2)
      md = ds2.GetMetadata('COLOR_PROFILE')
      ds = None
      ds2 = None
      self.assertEqual(md['SOURCE_ICC_PROFILE'], icc)

      with gdal.Open(png_path) as ds2:
        md = ds2.GetMetadata('COLOR_PROFILE')
        self.assertEqual(md['SOURCE_ICC_PROFILE'], icc)

  def test_png_copy_options_colorimetric_data(self) -> None:
    source_primaries = [(0.64, 0.33, 1.0), (0.3, 0.6, 1.0), (0.15, 0.06, 1.0)]
    source_whitepoint = (0.31271, 0.32902, 1.0)
    options = [
        'SOURCE_PRIMARIES_RED=' + _format_color_tuple(source_primaries[0]),
        'SOURCE_PRIMARIES_GREEN=' + _format_color_tuple(source_primaries[1]),
        'SOURCE_PRIMARIES_BLUE=' + _format_color_tuple(source_primaries[2]),
        'SOURCE_WHITEPOINT=' + _format_color_tuple(source_whitepoint),
        'PNG_GAMMA=1.5',
    ]

    driver_tiff = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/test_png_copy_options_colorimetric_data'
    ) as d:
      tiff_path = f'{d}/icc_test.tif'
      png_path = f'{d}/icc_test.png'

      ds = driver_tiff.Create(tiff_path, 64, 64, 3, gdal.GDT_Byte)
      ds2 = self.driver.CreateCopy(png_path, ds, options=options)
      self.assertIsNotNone(ds2)
      md = ds2.GetMetadata('COLOR_PROFILE')
      ds = None
      ds2 = None

      self._assert_colorimetric_md(md, source_whitepoint, source_primaries, 1.5)

      with gdal.Open(png_path) as ds2:
        md = ds2.GetMetadata('COLOR_PROFILE')
        self._assert_colorimetric_md(
            md, source_whitepoint, source_primaries, 1.5
        )

  def test_png_copy_colorimetric_data(self) -> None:
    source_primaries = [(0.64, 0.33, 1.0), (0.3, 0.6, 1.0), (0.15, 0.06, 1.0)]
    source_whitepoint = (0.31271, 0.32902, 1.0)
    tiff_options = [
        'SOURCE_PRIMARIES_RED=' + _format_color_tuple(source_primaries[0]),
        'SOURCE_PRIMARIES_GREEN=' + _format_color_tuple(source_primaries[1]),
        'SOURCE_PRIMARIES_BLUE=' + _format_color_tuple(source_primaries[2]),
        'SOURCE_WHITEPOINT=' + _format_color_tuple(source_whitepoint),
    ]

    driver_tiff = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/test_png_copy_colorimetric_data'
    ) as d:
      tiff_path = f'{d}/icc_test.tif'
      png_path = f'{d}/icc_test.png'

      ds = driver_tiff.Create(tiff_path, 64, 64, 3, gdal.GDT_Byte, tiff_options)
      ds = None

      with gdal.Open(tiff_path) as ds:
        ds2 = self.driver.CreateCopy(png_path, ds, options=['PNG_GAMMA=1.5'])
        self.assertIsNotNone(ds2)
        md = ds2.GetMetadata('COLOR_PROFILE')
        ds2 = None

      self._assert_colorimetric_md(md, source_whitepoint, source_primaries, 1.5)

      with gdal.Open(png_path) as ds2:
        md = ds2.GetMetadata('COLOR_PROFILE')
        self._assert_colorimetric_md(
            md, source_whitepoint, source_primaries, 1.5
        )

  def _assert_colorimetric_md(
      self,
      md: dict[str, str],
      source_whitepoint: tuple[float, float, float],
      source_primaries: list[tuple[float, float, float]],
      gamma: float,
  ) -> None:
    self.assertIterAlmostEqual(
        _parse_color_tuple(md['SOURCE_WHITEPOINT']),
        source_whitepoint,
        delta=0.0001,
    )
    for key, expected in zip(
        [
            'SOURCE_PRIMARIES_RED',
            'SOURCE_PRIMARIES_GREEN',
            'SOURCE_PRIMARIES_BLUE',
        ],
        source_primaries,
        strict=True,
    ):
      self.assertIterAlmostEqual(
          _parse_color_tuple(md[key]), expected, delta=0.0001
      )
    self.assertAlmostEqual(float(md['PNG_GAMMA']), gamma)

  def test_png_srgb(self) -> None:
    driver_tiff = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/test_png_srgb') as d:
      tiff_path = f'{d}/icc_test.tif'
      png_path = f'{d}/icc_test.png'

      ds = driver_tiff.Create(tiff_path, 64, 64, 3, gdal.GDT_Byte)
      ds2 = self.driver.CreateCopy(
          png_path, ds, options=['SOURCE_ICC_PROFILE_NAME=sRGB']
      )
      self.assertIsNotNone(ds2)
      md = ds2.GetMetadata('COLOR_PROFILE')
      ds = None
      ds2 = None
      self.assertEqual(md['SOURCE_ICC_PROFILE_NAME'], 'sRGB')

      with gdal.Open(png_path) as ds2:
        md = ds2.GetMetadata('COLOR_PROFILE')
        self.assertEqual(md['SOURCE_ICC_PROFILE_NAME'], 'sRGB')


if __name__ == '__main__':
  googletest.main()
