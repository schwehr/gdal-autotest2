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
# This is a rewrite of a file licensed as follows:
#
# Copyright (c) 2014, Even Rouault <even dot rouault at spatialys dot com>
#
# SPDX-License-Identifier: MIT
"""Test the GeoPackage (gpkg) raster driver.

Format is described here:

https://gdal.org/drivers/raster/gpkg.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/gpkg.py
"""
from collections.abc import Iterator
import contextlib
import importlib
import os
import pathlib
import re
import subprocess
from typing import Any

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.GPKG_DRIVER
EXT = '.gpkg'

_VALIDATE_GPKG: Any = importlib.import_module(
    'google3.third_party.gdal.swig.python.'
    'gdal-utils.osgeo_utils.samples.validate_gpkg'
)


@contextlib.contextmanager
def quiet_error_handler() -> Iterator[None]:
  gdal.PushErrorHandler('CPLQuietErrorHandler')
  try:
    yield
  finally:
    gdal.PopErrorHandler()


@contextlib.contextmanager
def no_gdal_exceptions() -> Iterator[None]:
  with gdal.ExceptionMgr(useExceptions=False):
    yield


@contextlib.contextmanager
def set_cache_max(byte_limit: int) -> Iterator[None]:
  old_cache_max = gdal.GetCacheMax()
  gdal.SetCacheMax(byte_limit)
  try:
    yield
  finally:
    gdal.SetCacheMax(old_cache_max)


@contextlib.contextmanager
def deregister_driver(drv: gdal.Driver) -> Iterator[None]:
  drv.Deregister()
  try:
    yield
  finally:
    drv.Register()


class GpkgTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    gdal.UseExceptions()
    self.enterContext(
        gdal.config_options({
            'OGR_SQLITE_SYNCHRONOUS': 'OFF',
            'GPKG_DEBUG': 'ON',
        })
    )
    self.png_dr = gdal.GetDriverByName('PNG')
    self.jpeg_dr = gdal.GetDriverByName('JPEG')
    self.webp_dr = gdal.GetDriverByName('WEBP')
    webp_md = self.webp_dr.GetMetadataItem('DMD_CREATIONOPTIONLIST') or ''
    self.webp_supports_rgba = 'LOSSLESS' in webp_md

    safe_id = re.sub(r'[^a-zA-Z0-9_]', '_', self.id().split('.')[-1])
    self.vsimem_dir = pathlib.PurePosixPath(f'/vsimem/{safe_id}')
    gdal.Mkdir(str(self.vsimem_dir), 0o755)
    self.addCleanup(gdal.RmdirRecursive, str(self.vsimem_dir))

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def validate(self, filename: str | os.PathLike[str]) -> None:
    path_str = str(filename)
    if path_str.startswith('/vsimem/'):
      stat = gdal.VSIStatL(path_str)
      if stat is None:
        self.fail(f'Missing /vsimem/ file: {path_str}')
      content = gcore_util.vsi_read_file_as_bytes(path_str, stat.size)
      real_path = self.create_tempfile('validate.gpkg').full_path
      pathlib.Path(real_path).write_bytes(content)
    else:
      real_path = path_str
    _VALIDATE_GPKG.check(real_path, extra_checks=True, warning_as_error=True)

  def get_expected_checksums(
      self,
      src_ds: gdal.Dataset,
      tile_drv: gdal.Driver,
      working_bands: int,
      extend_src: bool = True,
      clamp_output: bool = True,
  ) -> list[int]:
    mem_drv = gdal.GetDriverByName('MEM')
    if extend_src:
      mem_ds = mem_drv.Create('', 256, 256, working_bands)
    else:
      mem_ds = mem_drv.Create(
          '', src_ds.RasterXSize, src_ds.RasterYSize, working_bands
      )
    for i in range(working_bands):
      if src_ds.RasterCount == 2 and working_bands == 3:
        src_band = 1
      elif src_ds.RasterCount == 2 and working_bands == 4:
        src_band = 1 if i < 3 else 2
      elif src_ds.RasterCount == 1:
        src_band = 1
      else:
        src_band = i + 1
      data = src_ds.GetRasterBand(src_band).ReadRaster(
          0, 0, src_ds.RasterXSize, src_ds.RasterYSize
      )
      mem_ds.GetRasterBand(i + 1).WriteRaster(
          0, 0, src_ds.RasterXSize, src_ds.RasterYSize, data
      )
    options = [] if tile_drv.ShortName == 'PNG' else ['QUALITY=75']
    tmp_path = str(self.vsimem_dir / f'expected_cs.{tile_drv.ShortName}')
    try:
      tmp_ds = tile_drv.CreateCopy(tmp_path, mem_ds, options=options)
      if clamp_output:
        clamped_ds = mem_drv.Create(
            '', src_ds.RasterXSize, src_ds.RasterYSize, working_bands
        )
        clamped_ds.WriteRaster(
            0,
            0,
            src_ds.RasterXSize,
            src_ds.RasterYSize,
            tmp_ds.ReadRaster(0, 0, src_ds.RasterXSize, src_ds.RasterYSize),
        )
        expected_cs = [
            clamped_ds.GetRasterBand(i + 1).Checksum()
            for i in range(working_bands)
        ]
        clamped_ds = None  # pylint: disable=unused-variable
      else:
        tmp_ds.FlushCache()
        expected_cs = [
            tmp_ds.GetRasterBand(i + 1).Checksum() for i in range(working_bands)
        ]
      tmp_ds = None  # pylint: disable=unused-variable
      mem_ds = None  # pylint: disable=unused-variable
    finally:
      with no_gdal_exceptions():
        gdal.Unlink(tmp_path)
    return expected_cs

  def check_tile_format(
      self,
      out_ds: gdal.Dataset,
      expected_format: str | None,
      expected_band_count: int | None,
      expected_ct: bool | None,
      row: int = 0,
      col: int = 0,
      zoom_level: int | None = None,
      table_name: str = 'tmp',
  ) -> None:
    if zoom_level is None:
      zoom_level_str = f'(SELECT MAX(zoom_level) FROM {table_name})'
    else:
      zoom_level_str = str(zoom_level)
    sql = (
        'SELECT GDAL_GetMimeType(tile_data), GDAL_GetBandCount(tile_data), '
        f'GDAL_HasColorTable(tile_data) FROM {table_name} '
        f'WHERE zoom_level = {zoom_level_str} AND tile_column = {col} '
        f'AND tile_row = {row}'
    )
    sql_lyr = out_ds.ExecuteSQL(sql)
    try:
      feat = sql_lyr.GetNextFeature()
      if feat is not None:
        mime_type = feat.GetField(0)
        band_count = feat.GetField(1)
        has_ct = feat.GetField(2)
      else:
        mime_type = None
        band_count = None
        has_ct = None
    finally:
      out_ds.ReleaseResultSet(sql_lyr)

    if expected_format is None:
      self.assertIsNone(mime_type)
      return

    mime_map = {
        'PNG': 'image/png',
        'JPEG': 'image/jpeg',
        'WEBP': 'image/x-webp',
    }
    self.assertEqual(mime_type, mime_map[expected_format])
    self.assertEqual(band_count, expected_band_count)
    self.assertEqual(has_ct, expected_ct)

  def get_georeferenced_rgba_ds(
      self,
      alpha_fully_transparent: bool = False,
      alpha_fully_opaque: bool = False,
  ) -> gdal.Dataset:
    self.assertFalse(alpha_fully_transparent and alpha_fully_opaque)
    src_path = gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    with gdal.Open(src_path) as src_ds:
      tmp_path = str(self.vsimem_dir / 'rgba_tmp.tif')
      tmp_ds = gdal.GetDriverByName('GTiff').Create(
          tmp_path, src_ds.RasterXSize, src_ds.RasterYSize, 4
      )
      tmp_ds.SetGeoTransform([0, 10, 0, 0, 0, -10])
      tmp_ds.WriteRaster(
          0,
          0,
          src_ds.RasterXSize,
          src_ds.RasterYSize,
          src_ds.ReadRaster(0, 0, src_ds.RasterXSize, src_ds.RasterYSize),
      )
      if alpha_fully_opaque:
        tmp_ds.GetRasterBand(4).Fill(255)
      elif alpha_fully_transparent:
        tmp_ds.GetRasterBand(4).Fill(0)
      return tmp_ds

  def get_georeferenced_ds_with_pct32(self) -> gdal.Dataset:
    src_path = gcore_util.GetTestFilePath('stefan_full_rgba_pct32.png')
    with gdal.Open(src_path) as src_ds:
      tmp_path = str(self.vsimem_dir / 'pct32_tmp.tif')
      tmp_ds = gdal.GetDriverByName('GTiff').Create(
          tmp_path, src_ds.RasterXSize, src_ds.RasterYSize
      )
      tmp_ds.SetGeoTransform([0, 10, 0, 0, 0, -10])
      tmp_ds.GetRasterBand(1).SetColorTable(
          src_ds.GetRasterBand(1).GetColorTable()
      )
      tmp_ds.WriteRaster(
          0,
          0,
          src_ds.RasterXSize,
          src_ds.RasterYSize,
          src_ds.ReadRaster(0, 0, src_ds.RasterXSize, src_ds.RasterYSize),
      )
      return tmp_ds

  def get_georeferenced_greyalpha_ds(self) -> gdal.Dataset:
    src_path = gdrivers_util.GetTestFilePath('gtiff/stefan_full_greyalpha.tif')
    with gdal.Open(src_path) as src_ds:
      tmp_path = str(self.vsimem_dir / 'greyalpha_tmp.tif')
      tmp_ds = gdal.GetDriverByName('GTiff').Create(
          tmp_path, src_ds.RasterXSize, src_ds.RasterYSize, 2
      )
      tmp_ds.SetGeoTransform([0, 10, 0, 0, 0, -10])
      tmp_ds.WriteRaster(
          0,
          0,
          src_ds.RasterXSize,
          src_ds.RasterYSize,
          src_ds.ReadRaster(0, 0, src_ds.RasterXSize, src_ds.RasterYSize),
      )
      return tmp_ds

  def _tile_driver_and_bands(
      self, tile_drv_name: str
  ) -> tuple[gdal.Driver, int]:
    if tile_drv_name == 'PNG':
      return self.png_dr, 4
    if tile_drv_name == 'JPEG':
      return self.jpeg_dr, 3
    if tile_drv_name == 'WEBP':
      return self.webp_dr, 4 if self.webp_supports_rgba else 3
    raise ValueError(f'Unexpected tile driver: {tile_drv_name}')

  def test_gpkg_1(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
      expected_cs = ds.GetRasterBand(1).Checksum()
      clamped_expected_cs = self.get_expected_checksums(
          ds, self.png_dr, 1, clamp_output=False
      )[0]
      expected_gt = ds.GetGeoTransform()
      expected_wkt = ds.GetProjectionRef()
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=PNG', 'METADATA_TABLES=NO']
      ):
        pass

    self.validate(filename)

    with gdal.OpenEx(filename) as out_ds:
      self.assertEqual(out_ds.RasterCount, 4)
      got_gt = out_ds.GetGeoTransform()
      self.assertIterAlmostEqual(expected_gt, got_gt, delta=1e-8)
      self.assertEqual(out_ds.GetProjectionRef(), expected_wkt)
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [expected_cs, expected_cs, expected_cs, 4873])
      self.check_tile_format(out_ds, 'PNG', 2, False)

    # Check that there are no extensions.
    with gdal.Open(filename) as out_ds:
      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM sqlite_master WHERE type = 'table' "
          "AND name = 'gpkg_extensions'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      out_ds.ReleaseResultSet(sql_lyr)

      sql_lyr = out_ds.ExecuteSQL('SELECT * FROM gpkg_spatial_ref_sys')
      self.assertLess(
          sql_lyr.GetLayerDefn().GetFieldIndex('definition_12_063'), 0
      )
      out_ds.ReleaseResultSet(sql_lyr)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=3']) as out_ds:
      got_cs = [
          out_ds.GetRasterBand(i + 1).Checksum()
          for i in range(out_ds.RasterCount)
      ]
      self.assertEqual(got_cs, [expected_cs, expected_cs, expected_cs])

    with gdal.OpenEx(
        filename, open_options=['BAND_COUNT=4', 'USE_TILE_EXTENT=YES']
    ) as ds:
      self.assertEqual((ds.RasterXSize, ds.RasterYSize), (256, 256))
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(
          got_cs,
          [
              clamped_expected_cs,
              clamped_expected_cs,
              clamped_expected_cs,
              4898,
          ],
      )

    # Test USE_TILE_EXTENT=YES with empty table.
    with gdal.OpenEx(filename, gdal.OF_UPDATE) as ds:
      ds.ExecuteSQL('DELETE FROM tmp')
    with no_gdal_exceptions():
      ds = gdal.OpenEx(
          filename,
          gdal.OF_RASTER,
          open_options=['BAND_COUNT=4', 'USE_TILE_EXTENT=YES'],
      )
      self.assertIsNone(ds)

    gdal.Unlink(filename)

    # Without padding.
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
      expected_cs = ds.GetRasterBand(1).Checksum()
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=PNG', 'BLOCKSIZE=20']
      ):
        pass

    with gdal.Open(filename) as out_ds:
      self.assertEqual(out_ds.RasterCount, 1)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [expected_cs, expected_cs, expected_cs, 4873])
      self.check_tile_format(out_ds, 'PNG', 1, False)

  def test_gpkg_2(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
      expected_cs = self.get_expected_checksums(ds, self.jpeg_dr, 1)[0]
      clamped_expected_cs = self.get_expected_checksums(
          ds, self.jpeg_dr, 3, clamp_output=False
      )
      clamped_expected_cs.append(17849)
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=JPEG', 'METADATA_TABLES=NO']
      ):
        pass

    with gdal.Open(filename) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [expected_cs, expected_cs, expected_cs, 4873])
      self.check_tile_format(out_ds, 'JPEG', 1, False)

    with gdal.Open(filename) as out_ds:
      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM sqlite_master WHERE type = 'table' "
          "AND name = 'gpkg_extensions'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      out_ds.ReleaseResultSet(sql_lyr)

    with gdal.OpenEx(filename, open_options=['USE_TILE_EXTENT=YES']) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, clamped_expected_cs)

    gdal.Unlink(filename)

    # Without padding.
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
      expected_cs = self.get_expected_checksums(
          ds, self.jpeg_dr, 1, extend_src=False
      )[0]
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=JPEG', 'BLOCKSIZE=20']
      ):
        pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [expected_cs, expected_cs, expected_cs, 4873])
      self.check_tile_format(out_ds, 'JPEG', 1, False)

    # Try deregistering JPEG driver.
    with deregister_driver(self.jpeg_dr):
      with gdal.Open(filename) as out_ds:
        gdal.ErrorReset()
        with self.assertRaises(RuntimeError):
          out_ds.GetRasterBand(1).Checksum()
        self.assertNotEmpty(gdal.GetLastErrorMsg())

      gdal.Unlink(filename)

      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
        with self.driver.CreateCopy(
            filename, ds, options=['TILE_FORMAT=JPEG']
        ) as out_ds:
          gdal.ErrorReset()
          with self.assertRaises(RuntimeError):
            out_ds.FlushCache()
          self.assertNotEmpty(gdal.GetLastErrorMsg())

  def test_gpkg_3(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
      expected_cs = self.get_expected_checksums(ds, self.webp_dr, 3)
      clamped_expected_cs = self.get_expected_checksums(
          ds, self.webp_dr, 3, clamp_output=False
      )
      clamped_expected_cs.append(4898 if self.webp_supports_rgba else 17849)
      with self.driver.CreateCopy(filename, ds, options=['TILE_FORMAT=WEBP']):
        pass

    self.validate(filename)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
      self.assertIn(got_cs, (expected_cs, [4736, 4734, 4736]))

      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM gpkg_extensions WHERE table_name = 'tmp' "
          "AND column_name = 'tile_data' AND extension_name = 'gpkg_webp'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      out_ds.ReleaseResultSet(sql_lyr)

      expected_band_count = 4 if self.webp_supports_rgba else 3
      self.check_tile_format(out_ds, 'WEBP', expected_band_count, False)

    with gdal.OpenEx(
        filename, open_options=['BAND_COUNT=4', 'USE_TILE_EXTENT=YES']
    ) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertIn(got_cs, (clamped_expected_cs, [6850, 6848, 6850, 4898]))

    gdal.Unlink(filename)

    # Without padding.
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as ds:
      expected_cs = self.get_expected_checksums(
          ds, self.webp_dr, 3, extend_src=False
      )
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=WEBP', 'BLOCKSIZE=20']
      ):
        pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      expected_cs.append(4873)
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, expected_cs)
      self.check_tile_format(out_ds, 'WEBP', 3, False)

    # Try deregistering WEBP driver.
    with deregister_driver(self.webp_dr):
      # Should give warning at open time since the webp extension is declared.
      gdal.ErrorReset()
      with gdal.Open(filename) as out_ds:
        self.assertNotEmpty(gdal.GetLastErrorMsg())
        gdal.ErrorReset()
        with self.assertRaises(RuntimeError):
          out_ds.GetRasterBand(1).Checksum()
        self.assertNotEmpty(gdal.GetLastErrorMsg())

    gdal.Unlink(filename)

    # Check updating a non-WEBP dataset with TILE_FORMAT=WEBP.
    with self.driver.Create(filename, 1, 1) as out_ds:
      out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    with gdal.OpenEx(
        filename,
        gdal.OF_RASTER | gdal.OF_UPDATE,
        open_options=['TILE_FORMAT=WEBP'],
    ) as out_ds:
      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM gpkg_extensions WHERE table_name = 'tmp' "
          "AND column_name = 'tile_data' AND extension_name = 'gpkg_webp'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      out_ds.ReleaseResultSet(sql_lyr)

  @parameterized.named_parameters(
      ('png', 'PNG'),
      ('jpeg', 'JPEG'),
      ('webp', 'WEBP'),
  )
  def test_gpkg_4(self, tile_drv_name: str):
    tile_drv, working_bands = self._tile_driver_and_bands(tile_drv_name)
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    src_path = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')

    with gdal.Open(src_path) as ds:
      expected_cs = self.get_expected_checksums(ds, tile_drv, 3)
      clamped_expected_cs = self.get_expected_checksums(
          ds, tile_drv, 3, clamp_output=False
      )
      clamped_expected_cs.append(17849 if working_bands == 3 else 30638)
      with self.driver.CreateCopy(
          filename, ds, options=[f'TILE_FORMAT={tile_drv_name}']
      ):
        pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      expected_cs.append(30658)
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertIn(
          got_cs,
          (
              expected_cs,
              [22290, 21651, 21551, 30658],
              [22286, 21645, 21764, 30658],
          ),
      )
      self.check_tile_format(out_ds, tile_drv_name, working_bands, False)

    with gdal.OpenEx(
        filename, open_options=['BAND_COUNT=4', 'USE_TILE_EXTENT=YES']
    ) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertIn(
          got_cs,
          (
              clamped_expected_cs,
              [56886, 43228, 56508, 30638],
              [30478, 31718, 31360, 30638],
          ),
      )

    gdal.Unlink(filename)

    # Without padding.
    with gdal.Open(src_path) as ds:
      expected_cs = self.get_expected_checksums(
          ds, tile_drv, 3, extend_src=False
      )
      expected_cs.append(30658)
      with self.driver.CreateCopy(
          filename,
          ds,
          options=[f'TILE_FORMAT={tile_drv_name}', 'BLOCKSIZE=50'],
      ):
        pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, expected_cs)
      self.check_tile_format(out_ds, tile_drv_name, 3, False)

  @parameterized.parameters(['PNG', 'JPEG', 'WEBP'])
  def test_gpkg_7(self, tile_drv_name: str):
    tile_drv, working_bands = self._tile_driver_and_bands(tile_drv_name)
    filename = str(self.vsimem_dir / 'tmp.gpkg')

    src_ds = self.get_georeferenced_rgba_ds()
    with self.driver.CreateCopy(
        filename, src_ds, options=[f'TILE_FORMAT={tile_drv_name}']
    ):
      pass
    expected_cs = self.get_expected_checksums(src_ds, tile_drv, working_bands)
    src_filename = src_ds.GetDescription()
    src_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(src_filename)

    with gdal.Open(filename) as out_ds:
      got_cs = [
          out_ds.GetRasterBand(i + 1).Checksum() for i in range(working_bands)
      ]
      self.assertEqual(got_cs, expected_cs)
      self.check_tile_format(out_ds, tile_drv_name, working_bands, False)

    gdal.Unlink(filename)

    # Without padding with alpha fully opaque.
    tmp_ds = self.get_georeferenced_rgba_ds(alpha_fully_opaque=True)
    expected_cs = self.get_expected_checksums(
        tmp_ds, tile_drv, 3, extend_src=False
    )
    tmp_filename = tmp_ds.GetDescription()
    with self.driver.CreateCopy(
        filename,
        tmp_ds,
        options=[
            f'TILE_FORMAT={tile_drv_name}',
            f'BLOCKXSIZE={tmp_ds.RasterXSize}',
            f'BLOCKYSIZE={tmp_ds.RasterYSize}',
        ],
    ):
      pass
    tmp_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(tmp_filename)

    with gdal.Open(filename) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
      self.assertEqual(got_cs, expected_cs)
      self.check_tile_format(out_ds, tile_drv_name, 3, False)

    gdal.Unlink(filename)

    # Without padding with alpha fully transparent.
    tmp_ds = self.get_georeferenced_rgba_ds(alpha_fully_transparent=True)
    tmp_filename = tmp_ds.GetDescription()
    with self.driver.CreateCopy(
        filename,
        tmp_ds,
        options=[
            f'TILE_FORMAT={tile_drv_name}',
            f'BLOCKXSIZE={tmp_ds.RasterXSize}',
            f'BLOCKYSIZE={tmp_ds.RasterYSize}',
        ],
    ):
      pass
    tmp_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(tmp_filename)

    with gdal.Open(filename) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [0, 0, 0, 0])
      self.check_tile_format(out_ds, None, None, None)

  def test_gpkg_10(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    tmp_ds = self.get_georeferenced_ds_with_pct32()
    expected_ct = tmp_ds.GetRasterBand(1).GetColorTable().Clone()
    expected_cs_single_band = tmp_ds.GetRasterBand(1).Checksum()
    with self.driver.CreateCopy(
        filename,
        tmp_ds,
        options=[
            f'BLOCKXSIZE={tmp_ds.RasterXSize}',
            f'BLOCKYSIZE={tmp_ds.RasterYSize}',
        ],
    ):
      pass
    tmp_filename = tmp_ds.GetDescription()
    tmp_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(tmp_filename)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      self.assertEqual(
          out_ds.GetRasterBand(1).GetBlockSize(),
          [out_ds.RasterXSize, out_ds.RasterYSize],
      )
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [10991, 57677, 34965, 10638])
      self.check_tile_format(out_ds, 'PNG', 1, True)
      self.assertIsNone(out_ds.GetRasterBand(1).GetColorTable())

      with self.assertRaises(RuntimeError):
        out_ds.GetRasterBand(1).SetColorTable(None)

    with gdal.OpenEx(filename) as out_ds:
      got_cs = [
          out_ds.GetRasterBand(i + 1).Checksum()
          for i in range(out_ds.RasterCount)
      ]
      self.assertEqual(got_cs, [expected_cs_single_band])
      got_ct = out_ds.GetRasterBand(1).GetColorTable()
      self.assertEqual(expected_ct.GetCount(), got_ct.GetCount())
      self.assertEqual(expected_ct.GetColorEntry(0), got_ct.GetColorEntry(0))

      with self.assertRaises(RuntimeError):
        out_ds.GetRasterBand(1).SetColorTable(None)

    gdal.Unlink(filename)

    # Repeated SetColorTable().
    out_ds = self.driver.Create(filename, 1, 1)
    out_ds.GetRasterBand(1).SetColorTable(None)
    with self.assertRaises(RuntimeError):
      out_ds.GetRasterBand(1).SetColorTable(None)
    with self.assertRaises(RuntimeError):
      out_ds.Close()
    out_ds = None  # pylint: disable=unused-variable

    gdal.Unlink(filename)

    # Partial tile.
    tmp_ds = self.get_georeferenced_ds_with_pct32()
    with self.driver.CreateCopy(filename, tmp_ds):
      pass
    tmp_filename = tmp_ds.GetDescription()
    tmp_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(tmp_filename)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [10991, 57677, 34965, 10638])
      self.check_tile_format(out_ds, 'PNG', 4, False)
      self.assertIsNone(out_ds.GetRasterBand(1).GetColorTable())

  @parameterized.parameters(['JPEG', 'WEBP'])
  def test_gpkg_11(self, tile_drv_name: str):
    tile_drv, working_bands = self._tile_driver_and_bands(tile_drv_name)
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    pct_path = gcore_util.GetTestFilePath('stefan_full_rgba_pct32.png')
    bands_xml = ''.join(
        f'  <VRTRasterBand dataType="Byte" band="{i + 1}">\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename relativeToVRT="0">{pct_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        f'      <ColorTableComponent>{i + 1}</ColorTableComponent>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        for i in range(4)
    )
    rgba_xml = (
        '<VRTDataset rasterXSize="162"'
        f' rasterYSize="150">\n{bands_xml}</VRTDataset>'
    )
    with gdal.Open(rgba_xml) as rgba_ds:
      expected_cs = self.get_expected_checksums(
          rgba_ds, tile_drv, working_bands
      )

    tmp_ds = self.get_georeferenced_ds_with_pct32()
    with self.driver.CreateCopy(
        filename, tmp_ds, options=[f'TILE_FORMAT={tile_drv_name}']
    ):
      pass
    tmp_filename = tmp_ds.GetDescription()
    tmp_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(tmp_filename)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [
          out_ds.GetRasterBand(i + 1).Checksum() for i in range(working_bands)
      ]
      self.assertEqual(got_cs, expected_cs)

  def test_gpkg_13(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    src_path = gcore_util.GetTestFilePath('small_world_pct.tif')

    with gdal.Open(src_path) as src_ds:
      expected_ct = src_ds.GetRasterBand(1).GetColorTable().Clone()
      expected_cs_single_band = src_ds.GetRasterBand(1).Checksum()
      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=[
              f'BLOCKXSIZE={src_ds.RasterXSize}',
              f'BLOCKYSIZE={src_ds.RasterYSize}',
          ],
      ):
        pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
      self.assertEqual(got_cs, [63025, 48175, 12204])
      self.assertIsNone(out_ds.GetRasterBand(1).GetColorTable())

    with gdal.OpenEx(filename) as out_ds:
      got_cs = [
          out_ds.GetRasterBand(i + 1).Checksum()
          for i in range(out_ds.RasterCount)
      ]
      self.assertEqual(got_cs, [expected_cs_single_band])
      got_ct = out_ds.GetRasterBand(1).GetColorTable()
      self.assertEqual(expected_ct.GetCount(), got_ct.GetCount())

    gdal.Unlink(filename)

    # Partial tile.
    with gdal.Open(src_path) as src_ds:
      with self.driver.CreateCopy(filename, src_ds):
        pass

    with gdal.OpenEx(
        filename, open_options=['BAND_COUNT=4', 'USE_TILE_EXTENT=YES']
    ) as ds:
      self.assertEqual((ds.RasterXSize, ds.RasterYSize), (512, 256))
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [62358, 45823, 12238, 64301])

  def test_gpkg_14(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    small_world_path = gdrivers_util.GetTestFilePath('gtiff/small_world.tif')

    with gdal.Open(small_world_path) as src_ds:
      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=[
              'TILE_FORMAT=PNG',
              'RASTER_TABLE=foo',
              'RASTER_IDENTIFIER=bar',
              'RASTER_DESCRIPTION=baz',
          ],
      ):
        pass

      with self.assertRaises(RuntimeError):
        gdal.OpenEx(filename, open_options=['TABLE=non_existing'])

      with gdal.Open(filename) as ds:
        sql_lyr = ds.ExecuteSQL(
            "SELECT * FROM gpkg_contents WHERE table_name='foo'"
        )
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)
        ds.ReleaseResultSet(sql_lyr)
        self.assertEqual(ds.GetMetadataItem('IDENTIFIER'), 'bar')
        self.assertEqual(ds.GetMetadataItem('DESCRIPTION'), 'baz')
        self.assertEqual(ds.GetMetadataItem('ZOOM_LEVEL'), '1')
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)
        self.assertIsNone(ds.GetRasterBand(1).GetOverview(0))

      # In update mode, expose even empty overview levels.
      with gdal.Open(filename, gdal.GA_Update) as ds:
        self.assertEqual(ds.GetMetadataItem('ZOOM_LEVEL'), '1')
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
        self.assertIsNotNone(ds.GetRasterBand(1).GetOverview(0))
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 0)

      with gdal.OpenEx(filename, open_options=['ZOOM_LEVEL=2']) as ds:
        self.assertEqual(ds.RasterXSize, 400)

      with gdal.OpenEx(filename, open_options=['ZOOM_LEVEL=1']) as ds:
        self.assertEqual(ds.RasterXSize, 400)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)

      with gdal.OpenEx(
          filename, gdal.OF_UPDATE, open_options=['ZOOM_LEVEL=1']
      ) as ds:
        self.assertEqual(ds.RasterXSize, 400)
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)

      with gdal.OpenEx(filename, open_options=['ZOOM_LEVEL=0']) as ds:
        self.assertEqual(ds.RasterXSize, 200)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)

      tmp2_path = str(self.vsimem_dir / 'tmp2.gpkg')
      with gdal.Translate(
          tmp2_path,
          gdrivers_util.GetTestFilePath('gtiff/byte.tif'),
          format='GPKG',
      ):
        pass
      with gdal.OpenEx(tmp2_path, gdal.OF_UPDATE) as ds:
        ds.ExecuteSQL('UPDATE gpkg_contents SET min_x = NULL')
      with self.assertRaises(RuntimeError):
        gdal.OpenEx(tmp2_path, open_options=['ZOOM_LEVEL=-1'])
      gdal.Unlink(tmp2_path)

      with gdal.OpenEx(
          filename, open_options=['BAND_COUNT=4', 'USE_TILE_EXTENT=YES']
      ) as ds:
        self.assertEqual((ds.RasterXSize, ds.RasterYSize), (512, 256))
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [27644, 31968, 38564, 64301])

      # Open with exactly one tile shift.
      with gdal.OpenEx(
          filename,
          gdal.OF_UPDATE,
          open_options=[
              'BAND_COUNT=4',
              'TILE_FORMAT=PNG',
              'MINX=-410.4',
              'MAXY=320.4',
          ],
      ) as ds:
        self.assertEqual(
            (ds.RasterXSize, ds.RasterYSize), (400 + 256, 200 + 256)
        )
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [29070, 32796, 41086, 64288])
        data = ds.ReadRaster(0, 0, ds.RasterXSize, ds.RasterYSize)
        for i in range(ds.RasterCount):
          ds.GetRasterBand(i + 1).Fill(0)
        ds.FlushCache()
        sql_lyr = ds.ExecuteSQL('SELECT * FROM foo')
        fc = sql_lyr.GetFeatureCount()
        ds.ReleaseResultSet(sql_lyr)
        self.assertEqual(fc, 0)
        ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, data)

      # Partial tile shift (enclosing tiles).
      with gdal.OpenEx(
          f'GPKG:{filename}:foo',
          gdal.OF_UPDATE,
          open_options=[
              'BAND_COUNT=4',
              'MINX=-270',
              'MAXY=180',
              'MINY=-180',
              'MAXX=270',
          ],
      ) as ds:
        self.assertEqual((ds.RasterXSize, ds.RasterYSize), (600, 400))
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [28940, 32454, 40526, 64323])
        data = ds.ReadRaster(0, 0, ds.RasterXSize, ds.RasterYSize)
        for i in range(ds.RasterCount):
          ds.GetRasterBand(i + 1).Fill(0)
        ds.FlushCache()
        sql_lyr = ds.ExecuteSQL('SELECT * FROM foo')
        fc = sql_lyr.GetFeatureCount()
        ds.ReleaseResultSet(sql_lyr)
        self.assertEqual(fc, 0)
        ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, data)

      with gdal.OpenEx(
          f'GPKG:{filename}:foo',
          gdal.OF_UPDATE,
          open_options=[
              'BAND_COUNT=4',
              'MINX=-270',
              'MAXY=180',
              'MINY=-180',
              'MAXX=270',
          ],
      ) as ds:
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [28940, 32454, 40526, 64323])
        data = ds.GetRasterBand(1).ReadRaster(0, 0, 256, 256)
        ds.GetRasterBand(1).WriteRaster(0, 0, 256, 256, data)

      with gdal.OpenEx(
          f'GPKG:{filename}:foo',
          open_options=[
              'BAND_COUNT=4',
              'MINX=-270',
              'MAXY=180',
              'MINY=-180',
              'MAXX=270',
          ],
      ) as ds:
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [28940, 32454, 40526, 64323])

      # Partial tile shift (included in tiles).
      with gdal.OpenEx(
          f'GPKG:{filename}:foo',
          gdal.OF_UPDATE,
          open_options=[
              'BAND_COUNT=4',
              'MINX=-90',
              'MAXY=45',
              'MINY=-45',
              'MAXX=90',
          ],
      ) as ds:
        self.assertEqual((ds.RasterXSize, ds.RasterYSize), (200, 100))
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [9586, 9360, 26758, 48827])
        data = ds.ReadRaster(0, 0, ds.RasterXSize, ds.RasterYSize)
        ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, data)

      with gdal.OpenEx(
          f'GPKG:{filename}:foo',
          open_options=[
              'BAND_COUNT=4',
              'MINX=-90',
              'MAXY=45',
              'MINY=-45',
              'MAXX=90',
          ],
      ) as ds:
        self.assertEqual((ds.RasterXSize, ds.RasterYSize), (200, 100))
        got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
        self.assertEqual(got_cs, [9586, 9360, 26758, 48827])

      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=[
              'APPEND_SUBDATASET=YES',
              'RASTER_TABLE=other',
              'BLOCKSIZE=64',
              'TILE_FORMAT=PNG',
          ],
      ):
        pass

    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as byte_ds:
      with self.driver.CreateCopy(
          filename, byte_ds, options=['APPEND_SUBDATASET=YES']
      ):
        pass

    with gdal.Open(filename) as ds:
      md = ds.GetMetadata('SUBDATASETS')
      self.assertEqual(md['SUBDATASET_1_NAME'], f'GPKG:{filename}:foo')
      self.assertEqual(md['SUBDATASET_1_DESC'], 'foo - bar')
      self.assertEqual(md['SUBDATASET_2_NAME'], f'GPKG:{filename}:other')
      self.assertEqual(md['SUBDATASET_2_DESC'], 'other - other')
      self.assertEqual(md['SUBDATASET_3_NAME'], f'GPKG:{filename}:byte')
      self.assertEqual(md['SUBDATASET_3_DESC'], 'byte - byte')

    with gdal.Open(f'GPKG:{filename}:other') as ds:
      self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [64, 64])

    with gdal.OpenEx(
        filename,
        open_options=[
            'BAND_COUNT=4',
            'TABLE=other',
            'MINX=-90',
            'MAXY=45',
            'MINY=-45',
            'MAXX=90',
        ],
    ) as ds:
      self.assertEqual((ds.RasterXSize, ds.RasterYSize), (200, 100))
      self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [64, 64])
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [9586, 9360, 26758, 48827])

    gdal.Unlink(filename)

    # Open and fill with an AOI larger than the natural extent.
    with self.driver.Create(
        filename, 20, 20, 1, options=['BLOCKSIZE=20']
    ) as ds:
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    with gdal.OpenEx(
        filename,
        gdal.OF_UPDATE,
        open_options=[
            'MINX=-5',
            'MAXY=5',
            'MAXX=25',
            'MINY=-25',
            'BAND_COUNT=1',
        ],
    ) as ds:
      ds.GetRasterBand(1).Fill(255)

    with gdal.OpenEx(
        filename,
        open_options=['MINX=-10', 'MAXY=10', 'MINY=-30', 'MAXX=30'],
    ) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [4934, 4934, 4934, 4934])

    gdal.Unlink(filename)

    # Open and fill with an AOI smaller than the natural extent.
    with self.driver.Create(
        filename, 20, 20, 1, options=['BLOCKSIZE=20']
    ) as ds:
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    with gdal.OpenEx(
        filename,
        gdal.OF_UPDATE,
        open_options=[
            'MINX=5',
            'MAXY=-5',
            'MAXX=15',
            'MINY=-15',
            'BAND_COUNT=1',
        ],
    ) as ds:
      ds.GetRasterBand(1).Fill(255)

    with gdal.OpenEx(
        filename,
        open_options=['MINX=-10', 'MAXY=10', 'MINY=-30', 'MAXX=30'],
    ) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [1223, 1223, 1223, 1223])

    gdal.Unlink(filename)

    # Block intersects a tile at the right of the raster size.
    with self.driver.Create(filename, 400, 200, 1) as ds:
      ds.SetGeoTransform([-180, 0.9, 0, 90, 0, -0.9])

    with gdal.OpenEx(
        filename,
        gdal.OF_UPDATE,
        open_options=[
            'MINX=-5',
            'MAXY=5',
            'MAXX=25',
            'MINY=-25',
            'BAND_COUNT=1',
        ],
    ) as ds:
      ds.GetRasterBand(1).Fill(255)

    with gdal.OpenEx(filename) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [13365, 13365, 13365, 13365])

    gdal.Unlink(filename)

    # Test reading block from partial tile database.
    with self.driver.Create(filename, 512, 256, 4) as ds:
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    with gdal.OpenEx(
        filename,
        gdal.OF_UPDATE,
        open_options=['MINX=-5', 'MAXY=5', 'TILE_FORMAT=PNG'],
    ) as ds:
      with gdal.GetDriverByName('MEM').Create('', 256, 256) as mem_ds:
        mem_ds.GetRasterBand(1).Fill(255)
        mem_ds.FlushCache()
        data = mem_ds.GetRasterBand(1).ReadRaster()
      ds.GetRasterBand(2).WriteRaster(0, 0, 256, 256, data)

      with set_cache_max(0):
        pass

      sql_lyr = ds.ExecuteSQL('SELECT * FROM tmp')
      fc = sql_lyr.GetFeatureCount()
      ds.ReleaseResultSet(sql_lyr)
      self.assertEqual(fc, 0)

      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [0, 56451, 0, 0])

    # Overflow in ComputeTileAndPixelShifts().
    with self.assertRaises(RuntimeError):
      gdal.OpenEx(filename, open_options=['MINX=-1e12', 'MAXX=-0.9999e12'])
    with self.assertRaises(RuntimeError):
      gdal.OpenEx(filename, open_options=['MINY=-1e12', 'MAXY=-0.9999e12'])

    with gdal.Translate(
        filename, gdrivers_util.GetTestFilePath('gtiff/byte.tif'), format='GPKG'
    ):
      pass
    with gdal.OpenEx(filename, gdal.OF_UPDATE) as ds:
      ds.ExecuteSQL(
          'UPDATE gpkg_contents SET min_x=-100000000002000, '
          'max_x=-100000000000000'
      )
    with self.assertRaises(RuntimeError):
      gdal.Open(filename)

  def test_gpkg_15(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')

    # SetGeoTransform() and SetProjection() on a non-raster GPKG.
    with self.driver.Create(filename, 0, 0, 0) as out_ds:
      self.assertIsNone(out_ds.GetGeoTransform(can_return_null=True))
      self.assertEqual(out_ds.GetProjectionRef(), '')
      with self.assertRaises(RuntimeError):
        out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      with self.assertRaises(RuntimeError):
        out_ds.SetProjection(srs.ExportToWkt())

    gdal.Unlink(filename)

    # Repeated SetGeoTransform().
    with self.driver.Create(filename, 1, 1) as out_ds:
      self.assertEqual(
          out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1]), gdal.CE_None
      )
      with self.assertRaises(RuntimeError):
        out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    # Repeated SetProjection().
    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertTrue(out_ds.GetSpatialRef().IsLocal())
      self.assertIn('Undefined Cartesian SRS', out_ds.GetProjectionRef())
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      self.assertEqual(out_ds.SetProjection(srs.ExportToWkt()), gdal.CE_None)
      self.assertIn('4326', out_ds.GetProjectionRef())

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertIn('4326', out_ds.GetProjectionRef())
      out_ds.SetProjection('')

    with gdal.Open(filename) as out_ds:
      self.assertIsNone(out_ds.GetSpatialRef())
      with self.assertRaises(RuntimeError):
        out_ds.SetProjection('')
      with self.assertRaises(RuntimeError):
        out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    gdal.Unlink(filename)

    # Test SetColorInterpretation().
    with self.driver.Create(filename, 1, 1) as out_ds:
      out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      self.assertEqual(
          out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_Undefined),
          gdal.CE_None,
      )
      self.assertEqual(
          out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_GrayIndex),
          gdal.CE_None,
      )
      self.assertEqual(
          out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_PaletteIndex),
          gdal.CE_None,
      )
      with quiet_error_handler():
        self.assertEqual(
            out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand),
            gdal.CE_Warning,
        )

    gdal.Unlink(filename)

    with self.driver.Create(filename, 1, 1, 3) as out_ds:
      out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      self.assertEqual(
          out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand),
          gdal.CE_None,
      )
      with quiet_error_handler():
        self.assertEqual(
            out_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_RedBand),
            gdal.CE_Warning,
        )

    gdal.Unlink(filename)

    with self.driver.Create(filename, 1, 1, 2) as out_ds:
      out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      self.assertEqual(
          out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_GrayIndex),
          gdal.CE_None,
      )
      with quiet_error_handler():
        self.assertEqual(
            out_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand),
            gdal.CE_Warning,
        )
      self.assertEqual(
          out_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_AlphaBand),
          gdal.CE_None,
      )
      with quiet_error_handler():
        self.assertEqual(
            out_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_RedBand),
            gdal.CE_Warning,
        )

  def test_gpkg_16(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with self.driver.Create(
        filename, 1, 1, 3, options=['TILE_FORMAT=JPEG']
    ) as out_ds:
      out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      out_ds.GetRasterBand(1).Fill(255)
      out_ds.GetRasterBand(1).FlushCache()
      out_ds.GetRasterBand(2).Fill(127)
      out_ds.GetRasterBand(3).Checksum()
      out_ds.GetRasterBand(2).FlushCache()

    with gdal.Open(filename) as out_ds:
      val1 = out_ds.GetRasterBand(1).ReadRaster(0, 0, 1, 1)[0]
      val2 = out_ds.GetRasterBand(2).ReadRaster(0, 0, 1, 1)[0]
      val3 = out_ds.GetRasterBand(3).ReadRaster(0, 0, 1, 1)[0]

    self.assertAlmostEqual(val1, 255, delta=1)
    self.assertAlmostEqual(val2, 127, delta=1)
    self.assertAlmostEqual(val3, 0, delta=1)

  def test_gpkg_17(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    byte_path = gdrivers_util.GetTestFilePath('gtiff/byte.tif')

    # Without padding, immediately after CreateCopy.
    with gdal.Open(byte_path) as ds:
      with self.driver.CreateCopy(
          filename, ds, options=['BLOCKSIZE=10']
      ) as out_ds:
        out_ds.BuildOverviews('NEAR', [2])

    self.validate(filename)

    with gdal.Open(filename) as out_ds:
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          out_ds.GetRasterBand(1).GetOverview(0).GetDataset().RasterCount, 1
      )
      self.assertEqual(out_ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)
      self.check_tile_format(out_ds, 'PNG', 1, False, zoom_level=0)
      self.assertIsNone(out_ds.GetRasterBand(1).GetOverview(0).GetColorTable())

    gdal.Unlink(filename)

    # Without padding, after reopening.
    with gdal.Open(byte_path) as ds:
      with self.driver.CreateCopy(filename, ds, options=['BLOCKSIZE=10']):
        pass
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_UPDATE) as out_ds:
      out_ds.BuildOverviews('NEAR', [2])

    with gdal.Open(filename) as out_ds:
      self.assertEqual(out_ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)
      self.check_tile_format(out_ds, 'PNG', 1, False, zoom_level=0)

    gdal.Unlink(filename)

    # Without padding, after reopening with METADATA_TABLES=NO.
    with gdal.Open(byte_path) as ds:
      with self.driver.CreateCopy(
          filename, ds, options=['BLOCKSIZE=10', 'METADATA_TABLES=NO']
      ):
        pass
    with gdal.OpenEx(
        filename,
        gdal.OF_RASTER | gdal.OF_UPDATE,
        open_options=['TILE_FORMAT=PNG'],
    ) as out_ds:
      out_ds.BuildOverviews('NEAR', [2])

    with gdal.Open(filename) as out_ds:
      self.assertEqual(out_ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)
      self.check_tile_format(out_ds, 'PNG', 3, False, zoom_level=0)

      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM sqlite_master WHERE type = 'table' "
          "AND name = 'gpkg_extensions'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      out_ds.ReleaseResultSet(sql_lyr)

    # Test clearing overviews.
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_UPDATE) as out_ds:
      out_ds.BuildOverviews('NONE', [])
    with gdal.Open(filename) as out_ds:
      self.assertEqual(out_ds.GetRasterBand(1).GetOverviewCount(), 0)

    # Test building on an overview dataset -> error.
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_UPDATE) as out_ds:
      with self.assertRaises(RuntimeError):
        out_ds.GetRasterBand(1).GetOverview(0).GetDataset().BuildOverviews(
            'NONE', []
        )

      # Test building overview factor 1 -> error.
      with self.assertRaises(RuntimeError):
        out_ds.BuildOverviews('NEAR', [1])

      # Test building non-supported overview levels -> error.
      with gdal.config_option('ALLOW_GPKG_ZOOM_OTHER_EXTENSION', 'NO'):
        with self.assertRaises(RuntimeError):
          out_ds.BuildOverviews('NEAR', [3])
        with self.assertRaises(RuntimeError):
          out_ds.BuildOverviews('NEAR', [2, 4])

    # Test building overviews on read-only dataset.
    with gdal.OpenEx(filename, gdal.OF_RASTER) as out_ds:
      with self.assertRaises(RuntimeError):
        out_ds.BuildOverviews('NEAR', [2])

  def test_gpkg_18(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    tmp_tif = str(self.vsimem_dir / 'tmp.tif')

    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    ) as ds:
      with self.driver.CreateCopy(
          filename,
          ds,
          options=['TILE_FORMAT=PNG', 'BLOCKXSIZE=100', 'BLOCKYSIZE=100'],
      ) as out_ds:
        out_ds.BuildOverviews('CUBIC', [2, 4])

      gtiff_drv = gdal.GetDriverByName('GTiff')
      with gtiff_drv.CreateCopy(tmp_tif, ds) as tmp_ds:
        tmp_ds.BuildOverviews('CUBIC', [2, 4])
        expected_cs_ov0 = [
            tmp_ds.GetRasterBand(i + 1).GetOverview(0).Checksum()
            for i in range(3)
        ]
        expected_cs_ov1 = [
            tmp_ds.GetRasterBand(i + 1).GetOverview(1).Checksum()
            for i in range(3)
        ]
      gtiff_drv.Delete(tmp_tif)

    with gdal.Open(filename) as out_ds:
      got_cs = [
          out_ds.GetRasterBand(i + 1).GetOverview(0).Checksum()
          for i in range(3)
      ]
      self.assertEqual(got_cs, expected_cs_ov0)
      got_cs = [
          out_ds.GetRasterBand(i + 1).GetOverview(1).Checksum()
          for i in range(3)
      ]
      self.assertEqual(got_cs, expected_cs_ov1)
      self.check_tile_format(out_ds, 'PNG', 3, False, zoom_level=1)
      self.check_tile_format(out_ds, 'PNG', 4, False, zoom_level=0)

    # Test gpkg_zoom_other extension.
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_UPDATE) as out_ds:
      with quiet_error_handler():
        ret = out_ds.BuildOverviews('NEAR', [3])
      self.assertEqual(ret, 0)
      self.assertEqual(out_ds.GetRasterBand(1).GetOverviewCount(), 3)
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(0).Checksum()
              for i in range(3)
          ],
          expected_cs_ov0,
      )
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(1).Checksum()
              for i in range(3)
          ],
          [24807, 25544, 34002],
      )
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(2).Checksum()
              for i in range(3)
          ],
          expected_cs_ov1,
      )

      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM gpkg_extensions WHERE table_name = 'tmp' "
          "AND extension_name = 'gpkg_zoom_other'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      out_ds.ReleaseResultSet(sql_lyr)

    with gdal.Open(filename) as out_ds:
      self.assertEqual(out_ds.GetRasterBand(1).GetOverviewCount(), 3)
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(0).Checksum()
              for i in range(3)
          ],
          expected_cs_ov0,
      )
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(1).Checksum()
              for i in range(3)
          ],
          [24807, 25544, 34002],
      )
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(2).Checksum()
              for i in range(3)
          ],
          expected_cs_ov1,
      )

    # Add terminating overview.
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_UPDATE) as out_ds:
      self.assertEqual(out_ds.BuildOverviews('NEAR', [8]), 0)
      self.assertEqual(
          [
              out_ds.GetRasterBand(i + 1).GetOverview(3).Checksum()
              for i in range(3)
          ],
          [12725, 12539, 13553],
      )

    gdal.Unlink(filename)

    # Terminating overview without gpkg_zoom_other.
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    ) as ds:
      with self.driver.CreateCopy(
          filename,
          ds,
          options=[
              'TILE_FORMAT=PNG',
              'BLOCKXSIZE=100',
              'BLOCKYSIZE=100',
              'METADATA_TABLES=NO',
          ],
      ) as out_ds:
        self.assertEqual(out_ds.BuildOverviews('NEAR', [8]), 0)

    with gdal.Open(filename) as out_ds:
      sql_lyr = out_ds.ExecuteSQL(
          "SELECT * FROM sqlite_master WHERE type = 'table' "
          "AND name = 'gpkg_extensions'"
      )
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      out_ds.ReleaseResultSet(sql_lyr)

  def test_gpkg_19(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    tmp_tif = str(self.vsimem_dir / 'tmp.tif')

    with gdal.Open(gcore_util.GetTestFilePath('small_world_pct.tif')) as ds:
      with self.driver.CreateCopy(
          filename,
          ds,
          options=['TILE_FORMAT=PNG', 'BLOCKXSIZE=100', 'BLOCKYSIZE=100'],
      ) as out_ds:
        out_ds.BuildOverviews('NEAR', [2, 4])

      gtiff_drv = gdal.GetDriverByName('GTiff')
      with gtiff_drv.CreateCopy(tmp_tif, ds) as tmp_ds:
        tmp_ds.BuildOverviews('NEAR', [2, 4])
        expected_cs_ov0 = [tmp_ds.GetRasterBand(1).GetOverview(0).Checksum()]
        expected_cs_ov1 = [tmp_ds.GetRasterBand(1).GetOverview(1).Checksum()]
      gtiff_drv.Delete(tmp_tif)

    with gdal.OpenEx(
        filename, gdal.OF_RASTER, open_options=['BAND_COUNT=1']
    ) as out_ds:
      self.assertIsNotNone(
          out_ds.GetRasterBand(1).GetOverview(0).GetColorTable()
      )
      self.assertEqual(
          [out_ds.GetRasterBand(1).GetOverview(0).Checksum()], expected_cs_ov0
      )
      self.assertEqual(
          [out_ds.GetRasterBand(1).GetOverview(1).Checksum()], expected_cs_ov1
      )
      self.check_tile_format(out_ds, 'PNG', 1, True, zoom_level=1)
      self.check_tile_format(out_ds, 'PNG', 4, False, zoom_level=0)

  def test_gpkg_20(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    small_world_path = gdrivers_util.GetTestFilePath('gtiff/small_world.tif')

    # Without padding, with small tiles (<=256x256).
    with gdal.Open(small_world_path) as ds:
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=PNG8', 'BLOCKSIZE=200']
      ):
        pass
    with gdal.OpenEx(
        filename, gdal.OF_RASTER, open_options=['BAND_COUNT=4']
    ) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [30875, 31451, 38110, 64269])
      self.check_tile_format(out_ds, 'PNG', 1, True)
    gdal.Unlink(filename)

    # Without padding, with big tiles (>256x256).
    with gdal.Open(small_world_path) as ds:
      with self.driver.CreateCopy(
          filename,
          ds,
          options=['TILE_FORMAT=PNG8', 'BLOCKXSIZE=400', 'BLOCKYSIZE=200'],
      ):
        pass
    with gdal.OpenEx(
        filename, gdal.OF_RASTER, open_options=['BAND_COUNT=4']
    ) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [27001, 30168, 34800, 64269])
      self.check_tile_format(out_ds, 'PNG', 1, True)
    gdal.Unlink(filename)

    # With and without padding, with small tiles.
    with gdal.Open(small_world_path) as ds:
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=PNG8', 'BLOCKSIZE=150']
      ):
        pass
    with gdal.OpenEx(
        filename, gdal.OF_RASTER, open_options=['BAND_COUNT=4']
    ) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [27718, 31528, 42062, 64269])
      self.check_tile_format(out_ds, 'PNG', 1, True)
      self.check_tile_format(out_ds, 'PNG', 4, False, row=0, col=2)
    gdal.Unlink(filename)

    # Without padding, small tiles, fewer than 256 colors.
    with gdal.GetDriverByName('MEM').Create('', 50, 50, 3) as ds:
      ds.GetRasterBand(1).Fill(1)
      ds.GetRasterBand(2).Fill(2)
      ds.GetRasterBand(3).Fill(3)
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      with self.driver.CreateCopy(
          filename, ds, options=['TILE_FORMAT=PNG8', 'BLOCKSIZE=50']
      ):
        pass

    with gdal.OpenEx(
        filename, gdal.OF_RASTER, open_options=['BAND_COUNT=4']
    ) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [2500, 5000, 7500, 30658])
      self.check_tile_format(out_ds, 'PNG', 1, True)
    with gdal.OpenEx(
        filename, gdal.OF_RASTER, open_options=['BAND_COUNT=1']
    ) as out_ds:
      self.assertEqual(out_ds.GetRasterBand(1).GetColorTable().GetCount(), 1)

  def test_gpkg_21(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')

    with self.driver.Create(filename, 1, 1) as out_ds:
      out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      self.assertLen(out_ds.GetMetadataDomainList(), 3)

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      sql_lyr = out_ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'gpkg_metadata'"
      )
      self.assertIsNone(sql_lyr.GetNextFeature())
      out_ds.ReleaseResultSet(sql_lyr)
      out_ds.SetMetadataItem('foo', 'bar')

    self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))

    foo_value = 'bar'
    for i in range(4):
      with gdal.Open(filename, gdal.GA_Update) as out_ds:
        self.assertEmpty(out_ds.GetMetadata('GEOPACKAGE'))
        self.assertEqual(out_ds.GetMetadataItem('foo'), foo_value)
        self.assertEqual(
            out_ds.GetMetadata(),
            {'foo': foo_value, 'IDENTIFIER': 'tmp', 'ZOOM_LEVEL': '0'},
        )

        sql_lyr = out_ds.ExecuteSQL('SELECT * FROM gpkg_metadata')
        feat = sql_lyr.GetNextFeature()
        self.assertEqual(feat.GetField('id'), 1)
        self.assertEqual(feat.GetField('md_scope'), 'dataset')
        self.assertEqual(feat.GetField('md_standard_uri'), 'http://gdal.org')
        self.assertEqual(feat.GetField('mime_type'), 'text/xml')
        expected_xml = (
            '<GDALMultiDomainMetadata>\n'
            '  <Metadata>\n'
            f'    <MDI key="foo">{foo_value}</MDI>\n'
            '  </Metadata>\n'
            '</GDALMultiDomainMetadata>\n'
        )
        self.assertEqual(feat.GetField('metadata'), expected_xml)
        out_ds.ReleaseResultSet(sql_lyr)

        sql_lyr = out_ds.ExecuteSQL('SELECT * FROM gpkg_metadata_reference')
        feat = sql_lyr.GetNextFeature()
        self.assertEqual(feat.GetField('reference_scope'), 'table')
        self.assertEqual(feat.GetField('table_name'), 'tmp')
        self.assertTrue(feat.IsFieldNull('column_name'))
        self.assertTrue(feat.IsFieldNull('row_id_value'))
        self.assertTrue(feat.IsFieldSet('timestamp'))
        self.assertEqual(feat.GetField('md_file_id'), 1)
        self.assertTrue(feat.IsFieldNull('md_parent_id'))
        out_ds.ReleaseResultSet(sql_lyr)

        if i == 1:
          out_ds.SetMetadataItem('foo', 'bar')
        elif i == 2:
          out_ds.SetMetadataItem('foo', 'baz')
          foo_value = 'baz'

    # Clear metadata.
    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      out_ds.SetMetadata(None)

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      for table in ('gpkg_metadata', 'gpkg_metadata_reference'):
        sql_lyr = out_ds.ExecuteSQL(f'SELECT * FROM {table}')
        self.assertIsNone(sql_lyr.GetNextFeature())
        out_ds.ReleaseResultSet(sql_lyr)

      out_ds.SetMetadataItem('IDENTIFIER', 'my_identifier')
      out_ds.SetMetadataItem('DESCRIPTION', 'my_description')

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertEqual(out_ds.GetMetadataItem('IDENTIFIER'), 'my_identifier')
      self.assertEqual(out_ds.GetMetadataItem('DESCRIPTION'), 'my_description')
      for table in ('gpkg_metadata', 'gpkg_metadata_reference'):
        sql_lyr = out_ds.ExecuteSQL(f'SELECT * FROM {table}')
        self.assertIsNone(sql_lyr.GetNextFeature())
        out_ds.ReleaseResultSet(sql_lyr)

      # Write metadata in global scope.
      out_ds.SetMetadata({'bar': 'foo'}, 'GEOPACKAGE')

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertEqual(out_ds.GetMetadataItem('bar', 'GEOPACKAGE'), 'foo')
      sql_lyr = out_ds.ExecuteSQL('SELECT * FROM gpkg_metadata')
      feat = sql_lyr.GetNextFeature()
      self.assertEqual(feat.GetField('id'), 1)
      self.assertEqual(feat.GetField('md_scope'), 'dataset')
      self.assertEqual(feat.GetField('md_standard_uri'), 'http://gdal.org')
      self.assertEqual(feat.GetField('mime_type'), 'text/xml')
      self.assertEqual(
          feat.GetField('metadata'),
          '<GDALMultiDomainMetadata>\n'
          '  <Metadata>\n'
          '    <MDI key="bar">foo</MDI>\n'
          '  </Metadata>\n'
          '</GDALMultiDomainMetadata>\n',
      )
      out_ds.ReleaseResultSet(sql_lyr)

      sql_lyr = out_ds.ExecuteSQL('SELECT * FROM gpkg_metadata_reference')
      feat = sql_lyr.GetNextFeature()
      self.assertEqual(feat.GetField('reference_scope'), 'geopackage')
      self.assertTrue(feat.IsFieldNull('table_name'))
      self.assertTrue(feat.IsFieldNull('column_name'))
      self.assertTrue(feat.IsFieldNull('row_id_value'))
      self.assertTrue(feat.IsFieldSet('timestamp'))
      self.assertEqual(feat.GetField('md_file_id'), 1)
      self.assertTrue(feat.IsFieldNull('md_parent_id'))
      out_ds.ReleaseResultSet(sql_lyr)

      out_ds.SetMetadataItem('bar', 'baz', 'GEOPACKAGE')

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertEqual(out_ds.GetMetadataItem('bar', 'GEOPACKAGE'), 'baz')
      out_ds.SetMetadata(None, 'GEOPACKAGE')

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertEmpty(out_ds.GetMetadata('GEOPACKAGE'))

      out_ds.SetMetadataItem('1', '2')
      out_ds.SetMetadataItem('3', '4', 'CUSTOM_DOMAIN')
      out_ds.SetMetadataItem('6', '7', 'GEOPACKAGE')
      # Non-GDAL metadata.
      out_ds.ExecuteSQL(
          "INSERT INTO gpkg_metadata VALUES (10, 'dataset', 'uri', "
          "'text/plain', 'my_metadata')"
      )
      out_ds.ExecuteSQL(
          'INSERT INTO gpkg_metadata_reference VALUES '
          "('geopackage',NULL,NULL,NULL,'2012-08-17T14:49:32.932Z',10,NULL)"
      )
      out_ds.ExecuteSQL(
          "INSERT INTO gpkg_metadata VALUES (11, 'dataset', 'uri', "
          "'text/plain', 'other_metadata')"
      )
      out_ds.ExecuteSQL(
          'INSERT INTO gpkg_metadata_reference VALUES '
          "('geopackage',NULL,NULL,NULL,'2012-08-17T14:49:32.932Z',11,NULL)"
      )
      out_ds.ExecuteSQL(
          "INSERT INTO gpkg_metadata VALUES (12, 'dataset', 'uri', "
          "'text/plain', 'my_metadata_local')"
      )
      out_ds.ExecuteSQL(
          'INSERT INTO gpkg_metadata_reference VALUES '
          "('table','tmp',NULL,NULL,'2012-08-17T14:49:32.932Z',12,NULL)"
      )
      out_ds.ExecuteSQL(
          "INSERT INTO gpkg_metadata VALUES (13, 'dataset', 'uri', "
          "'text/plain', 'other_metadata_local')"
      )
      out_ds.ExecuteSQL(
          'INSERT INTO gpkg_metadata_reference VALUES '
          "('table','tmp',NULL,NULL,'2012-08-17T14:49:32.932Z',13,NULL)"
      )

    for _ in range(2):
      with gdal.Open(filename, gdal.GA_Update) as out_ds:
        self.assertEqual(out_ds.GetMetadataItem('1'), '2')
        self.assertEqual(
            out_ds.GetMetadataItem('GPKG_METADATA_ITEM_1'),
            'my_metadata_local',
        )
        self.assertEqual(
            out_ds.GetMetadataItem('GPKG_METADATA_ITEM_2'),
            'other_metadata_local',
        )
        self.assertEqual(
            out_ds.GetMetadataItem('GPKG_METADATA_ITEM_1', 'GEOPACKAGE'),
            'my_metadata',
        )
        self.assertEqual(
            out_ds.GetMetadataItem('GPKG_METADATA_ITEM_2', 'GEOPACKAGE'),
            'other_metadata',
        )
        self.assertEqual(out_ds.GetMetadataItem('3', 'CUSTOM_DOMAIN'), '4')
        self.assertEqual(out_ds.GetMetadataItem('6', 'GEOPACKAGE'), '7')
        out_ds.SetMetadata(out_ds.GetMetadata())
        out_ds.SetMetadata(out_ds.GetMetadata('GEOPACKAGE'), 'GEOPACKAGE')

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      out_ds.SetMetadata(None)
      out_ds.SetMetadata(None, 'CUSTOM_DOMAIN')
      out_ds.SetMetadata(None, 'GEOPACKAGE')

    with gdal.Open(filename, gdal.GA_Update) as out_ds:
      self.assertEqual(
          out_ds.GetMetadataItem('GPKG_METADATA_ITEM_1', 'GEOPACKAGE'),
          'my_metadata',
      )
      sql_lyr = out_ds.ExecuteSQL('SELECT * FROM gpkg_metadata WHERE id < 10')
      self.assertIsNone(sql_lyr.GetNextFeature())
      out_ds.ReleaseResultSet(sql_lyr)
      sql_lyr = out_ds.ExecuteSQL(
          'SELECT * FROM gpkg_metadata_reference WHERE md_file_id < 10'
      )
      self.assertIsNone(sql_lyr.GetNextFeature())
      out_ds.ReleaseResultSet(sql_lyr)

  # test_gpkg_metadata_PAM - PAM_ENABLED is disabled in Google3.

  @parameterized.parameters(['PNG', 'JPEG', 'WEBP', None])
  def test_gpkg_22(self, tile_drv_name: str | None):
    if tile_drv_name is None:
      expected_cs = [2466, 10807]
      clamped_expected_cs = [1989, 1989, 1989, 11580]
    elif tile_drv_name == 'PNG':
      expected_cs = [1970, 10807]
      clamped_expected_cs = [2100, 2100, 2100, 11580]
    elif tile_drv_name == 'JPEG':
      expected_cs = [6782, 32706]
      clamped_expected_cs = [6538, 6538, 6538, 32744]
    elif tile_drv_name == 'WEBP':
      if self.webp_supports_rgba:
        expected_cs = [13112, 10807]
        clamped_expected_cs = [13380, 13380, 13380, 11580]
      else:
        expected_cs = [13112, 32706]
        clamped_expected_cs = [13380, 13380, 13380, 32744]
    else:
      raise ValueError(f'Unexpected tile driver: {tile_drv_name}')

    filename = str(self.vsimem_dir / 'tmp.gpkg')
    tmp_ds = self.get_georeferenced_greyalpha_ds()
    if tile_drv_name:
      options = [f'TILE_FORMAT={tile_drv_name}', 'BLOCKSIZE=16']
    else:
      options = ['BLOCKSIZE=16']
    with self.driver.CreateCopy(filename, tmp_ds, options=options):
      pass
    tmp_ds_filename = tmp_ds.GetDescription()
    tmp_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(tmp_ds_filename)

    expected_4band = [
        expected_cs[0],
        expected_cs[0],
        expected_cs[0],
        expected_cs[1],
    ]
    checks = [
        (
            ['BAND_COUNT=2'],
            expected_cs,
            ([4899, 10807], [6274, 10807], [17638, 10807]),
        ),
        (
            ['BAND_COUNT=4'],
            expected_4band,
            (
                [4899, 4899, 4899, 10807],
                [4899, 4984, 4899, 10807],
                [6274, 6274, 6274, 10807],
                [17638, 17631, 17638, 10807],
            ),
        ),
        (
            ['BAND_COUNT=4', 'USE_TILE_EXTENT=YES'],
            clamped_expected_cs,
            (
                [5266, 5266, 5266, 11580],
                [5266, 5310, 5266, 11580],
                [6436, 6436, 6436, 11580],
                [17007, 17000, 17007, 11580],
            ),
        ),
    ]
    for open_options, expected, webp_variants in checks:
      with gdal.OpenEx(filename, open_options=open_options) as out_ds:
        got_cs = [
            out_ds.GetRasterBand(i + 1).Checksum() for i in range(len(expected))
        ]
        if tile_drv_name == 'WEBP':
          self.assertIn(
              got_cs,
              (expected, *webp_variants),
              msg=f'open_options={open_options}',
          )
        else:
          self.assertEqual(got_cs, expected, msg=f'open_options={open_options}')

  @parameterized.parameters([
      ('CUSTOM', [4672, 4672, 4672, 4873], None),
      ('GoogleCRS84Quad', [3562, 3562, 3562, 3691], None),
      ('GoogleCRS84Quad', [3562, 3562, 3562, 3691], ['RESAMPLING=BILINEAR']),
      ('GoogleCRS84Quad', [3417, 3417, 3417, 3691], ['RESAMPLING=CUBIC']),
      (
          'GoogleCRS84Quad',
          [3562, 3562, 3562, 3691],
          ['ZOOM_LEVEL_STRATEGY=AUTO'],
      ),
      (
          'GoogleCRS84Quad',
          [14445, 14445, 14445, 14448],
          ['ZOOM_LEVEL_STRATEGY=UPPER'],
      ),
      ('GoogleCRS84Quad', [14445, 14445, 14445, 14448], ['ZOOM_LEVEL=12']),
      ('GoogleCRS84Quad', None, ['ZOOM_LEVEL=31']),
      (
          'GoogleCRS84Quad',
          [3562, 3562, 3562, 3691],
          ['ZOOM_LEVEL_STRATEGY=LOWER'],
      ),
      ('GoogleMapsCompatible', [4118, 4118, 4118, 4406], None),
      ('PseudoTMS_GlobalGeodetic', [3562, 3562, 3562, 3691], None),
      ('PseudoTMS_GlobalMercator', [4118, 4118, 4118, 4406], None),
  ])
  def test_gpkg_26(
      self,
      scheme: str,
      expected_cs: list[int] | None,
      other_options: list[str] | None,
  ):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    options = ['TILE_FORMAT=PNG', f'TILING_SCHEME={scheme}']
    if other_options:
      options.extend(other_options)

    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
      if expected_cs is None:
        with self.assertRaises(RuntimeError):
          self.driver.CreateCopy(filename, src_ds, options=options)
        return
      with self.driver.CreateCopy(filename, src_ds, options=options):
        pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as ds:
      self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Area')
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      for got, expect in zip(got_cs, expected_cs, strict=True):
        self.assertAlmostEqual(got, expect, delta=2)

  @parameterized.parameters([
      ('GoogleCRS84Quad', [42255, 47336, 24963, 35707]),
      ('GoogleMapsCompatible', [35429, 36787, 20035, 17849]),
  ])
  def test_gpkg_26_bis(self, scheme: str, expected_cs: list[int]):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    ) as src_ds:
      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=['TILE_FORMAT=PNG', f'TILING_SCHEME={scheme}'],
      ):
        pass

    with gdal.Open(filename) as ds:
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      for got, expect in zip(got_cs, expected_cs, strict=True):
        self.assertAlmostEqual(got, expect, delta=4)

  def test_gpkg_26_errors(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with self.assertRaises(RuntimeError):
      self.driver.Create(
          filename,
          1,
          1,
          1,
          options=['TILING_SCHEME=GoogleCRS84Quad', 'BLOCKSIZE=128'],
      )
    with no_gdal_exceptions():
      gdal.Unlink(filename)

    ds = self.driver.Create(
        filename, 1, 1, 1, options=['TILING_SCHEME=GoogleCRS84Quad']
    )
    self.assertIn('4326', ds.GetProjectionRef())
    with self.assertRaises(RuntimeError):
      ds.SetGeoTransform([0, 10, 0, 0, 0, -10])
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32630)
    with self.assertRaises(RuntimeError):
      ds.SetProjection(srs.ExportToWkt())
    with self.assertRaises(RuntimeError):
      ds.Close()
    ds = None  # pylint: disable=unused-variable
    gdal.Unlink(filename)

    # Test with a .json tile matrix set.
    linz_tms = gcore_util.GetTestFilePath('tms_LINZAntarticaMapTileGrid.json')
    with gdal.Translate(
        filename,
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif'),
        format='GPKG',
        creationOptions=[f'TILING_SCHEME={linz_tms}'],
        projWin=[-180, -50, 180, -90],
    ) as ds:
      self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), '5482')
      self.assertIterAlmostEqual(
          ds.GetGeoTransform(),
          (314023.27126670163, 28672, 0.0, 5685976.728733298, 0.0, -28672),
          delta=1e-8,
      )
    gdal.Unlink(filename)

    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
      # Unsupported TILING_SCHEME.
      nztm_tms = gcore_util.GetTestFilePath('tms_NZTM2000.json')
      with self.assertRaisesRegex(RuntimeError, 'Unsupported tiling scheme'):
        self.driver.CreateCopy(
            filename, src_ds, options=[f'TILING_SCHEME={nztm_tms}']
        )
      with no_gdal_exceptions():
        gdal.Unlink(filename)

      # Invalid TILING_SCHEME.
      with self.assertRaisesRegex(
          RuntimeError, 'Invalid tiling matrix set name'
      ):
        self.driver.CreateCopy(
            filename, src_ds, options=['TILING_SCHEME=invalid']
        )
      with no_gdal_exceptions():
        gdal.Unlink(filename)

      # Invalid target filename.
      with self.assertRaises(RuntimeError):
        self.driver.CreateCopy(
            '/foo/tmp.gpkg', src_ds, options=['TILING_SCHEME=GoogleCRS84Quad']
        )

    # Source is not georeferenced.
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/stefan_full_rgba.tif')
    ) as src_ds:
      with self.assertRaises(RuntimeError):
        self.driver.CreateCopy(
            filename, src_ds, options=['TILING_SCHEME=GoogleCRS84Quad']
        )

  def test_gpkg_27(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with set_cache_max(0):
      with gdal.Open(
          gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
      ) as src_ds:
        expected_cs = [src_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
        with self.driver.CreateCopy(
            filename,
            src_ds,
            options=['TILE_FORMAT=PNG', 'BLOCKXSIZE=200', 'BLOCKYSIZE=200'],
        ) as out_ds:
          got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
          self.assertEqual(got_cs, expected_cs)

  def test_gpkg_28(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    ) as src_ds:
      data = [src_ds.GetRasterBand(b + 1).ReadRaster() for b in range(3)]
      expected_cs = [src_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]

    with self.driver.Create(
        filename,
        400,
        200,
        3,
        options=['TILE_FORMAT=PNG', 'BLOCKXSIZE=400', 'BLOCKYSIZE=200'],
    ) as out_ds:
      out_ds.SetGeoTransform([0, 10, 0, 0, 0, -10])
      out_ds.GetRasterBand(1).WriteRaster(0, 0, 400, 200, data[0])
      with set_cache_max(0):
        pass
      out_ds.GetRasterBand(2).ReadRaster(0, 0, 400, 200)
      for b in range(2):
        out_ds.GetRasterBand(b + 2).WriteRaster(0, 0, 400, 200, data[b + 1])

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=3']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
      self.assertEqual(got_cs, expected_cs)

  @parameterized.parameters([0, 200])
  def test_gpkg_29(self, x: int):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    ) as src_ds:
      left = [
          src_ds.GetRasterBand(b + 1).ReadRaster(0, 0, 200, 200)
          for b in range(3)
      ]
      right = [
          src_ds.GetRasterBand(b + 1).ReadRaster(200, 0, 200, 200)
          for b in range(3)
      ]
      expected_cs = [src_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]

    with self.driver.Create(
        filename,
        400,
        200,
        3,
        options=['TILE_FORMAT=PNG', 'BLOCKXSIZE=200', 'BLOCKYSIZE=200'],
    ) as out_ds:
      out_ds.SetGeoTransform([0, 10, 0, 0, 0, -10])
      out_ds.GetRasterBand(1).WriteRaster(0, 0, 200, 200, left[0])
      with set_cache_max(0):
        pass
      out_ds.GetRasterBand(2).ReadRaster(x, 0, 200, 200)
      for b in range(2):
        out_ds.GetRasterBand(b + 2).WriteRaster(0, 0, 200, 200, left[b + 1])
      for b in range(3):
        out_ds.GetRasterBand(b + 1).WriteRaster(200, 0, 200, 200, right[b])

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=3']) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(3)]
      self.assertEqual(got_cs, expected_cs)

  def test_gpkg_31(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.config_option('GPKG_PNG_SUPPORTS_2BANDS', 'NO'):
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        with self.driver.CreateCopy(
            filename, src_ds, options=['TILE_FORMAT=PNG', 'BLOCKSIZE=21']
        ):
          pass

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as ds:
      self.check_tile_format(ds, 'PNG', 4, False)
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [4672, 4672, 4672, 4873])

  def test_gpkg_32(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    src_ds = self.get_georeferenced_greyalpha_ds()
    with gdal.config_option('GPKG_PNG_SUPPORTS_2BANDS', 'NO'):
      with self.driver.CreateCopy(
          filename, src_ds, options=['TILE_FORMAT=PNG', 'BLOCKSIZE=200']
      ):
        pass
    src_filename = src_ds.GetDescription()
    src_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(src_filename)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as ds:
      self.check_tile_format(ds, 'PNG', 4, False)
      got_cs = [
          ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)
      ]
      self.assertEqual(got_cs, [1970, 1970, 1970, 10807])

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=2']) as ds:
      got_cs = [
          ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)
      ]
      self.assertEqual(got_cs, [1970, 10807])

  def test_gpkg_33(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    src_ds = self.get_georeferenced_ds_with_pct32()
    with gdal.config_option('GPKG_PNG_SUPPORTS_CT', 'NO'):
      with self.driver.CreateCopy(
          filename, src_ds, options=['TILE_FORMAT=PNG']
      ):
        pass
    src_filename = src_ds.GetDescription()
    src_ds = None  # pylint: disable=unused-variable
    gdal.Unlink(src_filename)

    with gdal.OpenEx(filename, open_options=['BAND_COUNT=4']) as ds:
      self.check_tile_format(ds, 'PNG', 4, False)
      got_cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [10991, 57677, 34965, 10638])

  def test_gpkg_34(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.GetDriverByName('MEM').Create('', 512, 417) as src_ds:
      src_ds.SetGeoTransform([
          -20037508.342789299786091,
          2 * 20037508.342789299786091 / 512,
          0,
          16213801.067584000527859,
          0,
          -2 * 16213801.067584000527859 / 417,
      ])
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      src_ds.SetProjection(srs.ExportToWkt())
      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=['TILE_FORMAT=PNG', 'TILING_SCHEME=GoogleMapsCompatible'],
      ):
        pass

    with gdal.Open(filename, gdal.GA_Update) as ds:
      gdal.ErrorReset()
      ds.BuildOverviews('NEAR', [2])
    self.assertEmpty(gdal.GetLastErrorMsg())

  def test_gpkg_35(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with gdal.GetDriverByName('MEM').Create('', 512, 417, 4) as src_ds:
      src_ds.SetGeoTransform([
          -20037508.342789299786091,
          2 * 20037508.342789299786091 / 512,
          0,
          16213801.067584000527859,
          0,
          -2 * 16213801.067584000527859 / 417,
      ])
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      src_ds.SetProjection(srs.ExportToWkt())
      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=['TILE_FORMAT=PNG', 'TILING_SCHEME=GoogleMapsCompatible'],
      ) as out_ds:
        out_ds.GetRasterBand(1).Fill(32)
        out_ds.GetRasterBand(2).Fill(64)
        out_ds.GetRasterBand(3).Fill(128)
        out_ds.GetRasterBand(4).Fill(255)
        height = out_ds.RasterYSize
        expected_data = out_ds.ReadRaster(0, 0, 256, height)

    with gdal.GetDriverByName('MEM').Create('', 256, height, 4) as src_ds:
      for band_idx in range(1, 5):
        src_ds.GetRasterBand(band_idx).Fill(255)
      white_data = src_ds.ReadRaster(0, 0, 256, height)

    with gdal.Open(filename, gdal.GA_Update) as ds:
      ds.WriteRaster(256, 0, 256, height, white_data)
      with set_cache_max((256 * 256 + 2 * 128) * 4):
        got_data = ds.ReadRaster(0, 0, 256, height)

    self.assertEqual(got_data, expected_data)

  def test_gpkg_36(self):
    filename = str(self.vsimem_dir / 'gpkg_36.gpkg')
    with gdal.Open(gcore_util.GetTestFilePath('small_world_pct.tif')) as src_ds:
      with self.driver.CreateCopy(
          filename,
          src_ds,
          options=[
              'TILE_FORMAT=PNG',
              'TILING_SCHEME=GoogleMapsCompatible',
              'RESAMPLING=NEAREST',
          ],
      ):
        pass

    with gdal.Open(filename) as out_ds:
      got_cs = [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)]
      self.assertEqual(got_cs, [993, 50461, 64354, 17849])
      self.assertIsNone(out_ds.GetRasterBand(1).GetColorTable())

  def test_gpkg_37(self):
    filename = str(self.vsimem_dir / 'gpkg_37.gpkg')
    with self.driver.Create(filename, 205000, 200000) as ds:
      ds.SetGeoTransform([100, 0.000001, 0, 100, 0, -0.000001])

    with gdal.Open(filename, gdal.GA_Update) as ds:
      ret = ds.BuildOverviews(
          'NONE', [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]
      )
      self.assertEqual(ret, 0)
      self.assertEmpty(gdal.GetLastErrorMsg())

  def test_gpkg_38(self):
    filename = str(self.vsimem_dir / 'gpkg_38.gpkg')
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    ) as src_ds:
      with self.driver.CreateCopy(
          filename, src_ds, options=['TILE_FORMAT=PNG', 'BLOCKSIZE=8']
      ):
        pass

      with gdal.Open(filename) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).Checksum(),
            src_ds.GetRasterBand(1).Checksum(),
        )
      filesize = gdal.VSIStatL(filename).size
      gdal.Unlink(filename)

      for limit in (filesize - 100000, filesize - 1):
        limited_filename = f'/vsimem/||maxlength={limit}||gpkg_38.gpkg'
        try:
          with self.assertRaises(RuntimeError):
            with self.driver.CreateCopy(
                limited_filename,
                src_ds,
                options=['TILE_FORMAT=PNG', 'BLOCKSIZE=8'],
            ) as ds:
              ds.FlushCache()
        finally:
          with no_gdal_exceptions():
            gdal.Unlink(limited_filename)

  def test_gpkg_39(self):
    filename = str(self.vsimem_dir / 'gpkg_39.gpkg')
    int16_path = gdrivers_util.GetTestFilePath('gtiff/int16.tif')
    byte_path = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    float32_path = gdrivers_util.GetTestFilePath('gtiff/float32.tif')

    with gdal.Open(int16_path) as src_ds:
      with gdal.Translate(filename, src_ds, format='GPKG'):
        pass

    self.validate(filename)

    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Int16)
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Area')
      sql_lyr = ds.ExecuteSQL(
          'SELECT scale, offset FROM gpkg_2d_gridded_tile_ancillary'
      )
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['scale'], 1.0)
      self.assertEqual(f['offset'], 0.0)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          'SELECT grid_cell_encoding FROM gpkg_2d_gridded_coverage_ancillary'
      )
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['grid_cell_encoding'], 'grid-value-is-area')
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'gpkg_metadata'"
      )
      self.assertIsNone(sql_lyr.GetNextFeature())
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL('PRAGMA application_id')
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['application_id'], 1196444487)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL('PRAGMA user_version')
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['user_version'], 10400)
      ds.ReleaseResultSet(sql_lyr)

      self.assertEmpty(ds.GetRasterBand(1).GetMetadata())

    # From an AREA_OR_POINT=Point dataset.
    with gdal.GetDriverByName('MEM').Create(
        '', 2, 2, 1, gdal.GDT_Int16
    ) as src_ds2:
      src_ds2.SetGeoTransform([2, 1, 0, 49, 0, -1])
      src_ds2.SetMetadataItem('AREA_OR_POINT', 'Point')
      src_ds2.GetRasterBand(1).SetUnitType('m')
      with gdal.Translate(filename, src_ds2, format='GPKG'):
        pass
    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Point')
      self.assertEqual(ds.GetRasterBand(1).GetUnitType(), 'm')

    # Test GRID_CELL_ENCODING=grid-value-is-corner.
    with gdal.Translate(
        filename,
        byte_path,
        format='GPKG',
        outputType=gdal.GDT_UInt16,
        creationOptions=['GRID_CELL_ENCODING=grid-value-is-corner'],
    ):
      pass
    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Point')
      self.assertEqual(
          ds.GetRasterBand(1).GetMetadata(),
          {'GRID_CELL_ENCODING': 'grid-value-is-corner'},
      )
      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'gpkg_metadata'"
      )
      self.assertIsNone(sql_lyr.GetNextFeature())
      ds.ReleaseResultSet(sql_lyr)

    with gdal.Translate('', byte_path, format='MEM') as src_with_stats:
      src_with_stats.GetRasterBand(1).ComputeStatistics(approx_ok=False)
      expected_md = src_with_stats.GetRasterBand(1).GetMetadata()
      expected_md['GRID_CELL_ENCODING'] = 'grid-value-is-corner'
      with gdal.Translate(
          filename,
          src_with_stats,
          format='GPKG',
          outputType=gdal.GDT_UInt16,
          creationOptions=['GRID_CELL_ENCODING=grid-value-is-corner'],
      ):
        pass
    self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Point')
      self.assertEqual(ds.GetRasterBand(1).GetMetadata(), expected_md)

    with gdal.Open(int16_path) as src_ds:
      # With nodata: statistics available.
      with gdal.Translate(filename, src_ds, format='GPKG', noData=0):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadata(),
            {'STATISTICS_MINIMUM': '74', 'STATISTICS_MAXIMUM': '255'},
        )
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
      with gdal.Open(filename) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadataItem('STATISTICS_MINIMUM'), '74'
        )

      # Entire tile: statistics available.
      with gdal.Translate(
          filename, src_ds, format='GPKG', width=256, height=256
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadata(),
            {'STATISTICS_MINIMUM': '74', 'STATISTICS_MAXIMUM': '255'},
        )
      with gdal.Open(filename) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadataItem('STATISTICS_MINIMUM'), '74'
        )

      with gdal.Translate(filename, src_ds, format='GPKG', noData=1):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), -32768.0)

      with gdal.Translate(filename, src_ds, format='GPKG', noData=74):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4649)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), -32768.0)

      with gdal.Translate(
          filename,
          src_ds,
          format='GPKG',
          noData=1,
          creationOptions=['TILING_SCHEME=GoogleMapsCompatible'],
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertIn(ds.GetRasterBand(1).Checksum(), (4118, 4077))

      with gdal.config_option('GPKG_ADD_DEFINITION_12_063', 'YES'):
        with gdal.Translate(
            filename,
            src_ds,
            format='GPKG',
            noData=1,
            creationOptions=['TILING_SCHEME=GoogleMapsCompatible'],
        ):
          pass
      with gdal.Open(filename) as ds:
        self.assertIn(ds.GetRasterBand(1).Checksum(), (4118, 4077))

      with gdal.Translate(
          filename, src_ds, format='GPKG', width=1024, height=1024
      ):
        pass
      with gdal.Open(filename, gdal.GA_Update) as ds:
        ds.BuildOverviews('NEAR', [2, 4])
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 37308)
        ds.BuildOverviews('NONE', [])
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)

      with gdal.Translate(
          filename, src_ds, format='GPKG', outputType=gdal.GDT_UInt16
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        sql_lyr = ds.ExecuteSQL(
            'SELECT scale, offset FROM gpkg_2d_gridded_tile_ancillary'
        )
        self.assertEqual(sql_lyr.GetNextFeature()['scale'], 1.0)
        ds.ReleaseResultSet(sql_lyr)

      with gdal.Translate(
          filename,
          src_ds,
          format='GPKG',
          outputType=gdal.GDT_UInt16,
          noData=1,
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 1.0)

      with gdal.Translate(
          filename,
          src_ds,
          format='GPKG',
          outputType=gdal.GDT_UInt16,
          noData=74,
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 74.0)

    with gdal.Open(float32_path) as src_ds:
      with gdal.Translate(filename, src_ds, format='GPKG'):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float32)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        sql_lyr = ds.ExecuteSQL(
            'SELECT scale, offset FROM gpkg_2d_gridded_tile_ancillary'
        )
        self.assertEqual(sql_lyr.GetNextFeature().GetField('scale'), 1.0)
        ds.ReleaseResultSet(sql_lyr)

      with gdal.Translate(filename, src_ds, format='GPKG', noData=1):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 1)

      with gdal.Translate(
          filename,
          src_ds,
          format='GPKG',
          creationOptions=['TILE_FORMAT=PNG'],
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float32)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        sql_lyr = ds.ExecuteSQL(
            'SELECT scale, offset FROM gpkg_2d_gridded_tile_ancillary'
        )
        f = sql_lyr.GetNextFeature()
        self.assertNotEqual(f['scale'], 1.0)
        self.assertTrue(f.IsFieldSetAndNotNull('scale'))
        ds.ReleaseResultSet(sql_lyr)

      with gdal.Translate(
          filename,
          src_ds,
          format='GPKG',
          noData=74,
          creationOptions=['TILE_FORMAT=PNG'],
      ):
        pass
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float32)
        expected_nd = -3.4028234663852885981e38
        self.assertAlmostEqual(
            ds.GetRasterBand(1).GetNoDataValue(),
            expected_nd,
            delta=abs(expected_nd) * 1e-8,
        )
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4651)
        sql_lyr = ds.ExecuteSQL(
            'SELECT scale, offset FROM gpkg_2d_gridded_tile_ancillary'
        )
        f = sql_lyr.GetNextFeature()
        self.assertNotEqual(f['scale'], 1.0)
        self.assertTrue(f.IsFieldSetAndNotNull('scale'))
        ds.ReleaseResultSet(sql_lyr)

    asc_path = str(self.vsimem_dir / 'gpkg_39.asc')

    # Particular case with nodata = -32768 for Int16.
    gdal.FileFromMemBuffer(
        asc_path,
        'ncols        6\nnrows        1\nxllcorner    440720\n'
        'yllcorner    3750120\ncellsize     60\nNODATA_value -32768\n'
        ' -32768 -32767 -32766 0 32766 32767',
    )
    with gdal.Translate(
        filename, asc_path, format='GPKG', outputType=gdal.GDT_Int16
    ):
      pass
    with gdal.Open(asc_path) as src_ds, gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Int16)
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), -32768.0)
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
      )
    gdal.Unlink(asc_path)

    # Particular case with nodata = 65535 for UInt16.
    gdal.FileFromMemBuffer(
        asc_path,
        'ncols        6\nnrows        1\nxllcorner    440720\n'
        'yllcorner    3750120\ncellsize     60\nNODATA_value 65535\n'
        '0 1 2 65533 65534 65535',
    )
    with gdal.Translate(
        filename, asc_path, format='GPKG', outputType=gdal.GDT_UInt16
    ):
      pass
    with gdal.Open(asc_path) as src_ds, gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 65535.0)
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
      )
    gdal.Unlink(asc_path)

    # Particular case with nodata = 0 for UInt16.
    gdal.FileFromMemBuffer(
        asc_path,
        'ncols        6\nnrows        1\nxllcorner    440720\n'
        'yllcorner    3750120\ncellsize     60\nNODATA_value 0\n'
        '0 1 2 65533 65534 65535',
    )
    with gdal.Translate(
        filename, asc_path, format='GPKG', outputType=gdal.GDT_UInt16
    ):
      pass
    with gdal.Open(asc_path) as src_ds, gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 0)
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
      )
    gdal.Unlink(asc_path)

    # Test large dynamic for Float32 and TILE_FORMAT=PNG.
    gdal.FileFromMemBuffer(
        asc_path,
        'ncols        2\nnrows        1\nxllcorner    440720\n'
        'yllcorner    3750120\ncellsize     60\n-100000 100000',
    )
    with gdal.Translate(
        filename,
        asc_path,
        format='GPKG',
        outputType=gdal.GDT_Float32,
        creationOptions=['TILE_FORMAT=PNG'],
    ):
      pass
    self.validate(filename)
    with gdal.Open(asc_path) as src_ds, gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float32)
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
      )
    gdal.Unlink(asc_path)

    # Test large dynamic for Float32 and TILE_FORMAT=PNG and nodata.
    gdal.FileFromMemBuffer(
        asc_path,
        'ncols        2\nnrows        1\nxllcorner    440720\n'
        'yllcorner    3750120\ncellsize     60\n-100000 100000',
    )
    with gdal.Translate(
        filename,
        asc_path,
        format='GPKG',
        outputType=gdal.GDT_Float32,
        noData=0,
        creationOptions=['TILE_FORMAT=PNG'],
    ):
      pass
    with gdal.Open(asc_path) as src_ds, gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float32)
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), src_ds.GetRasterBand(1).Checksum()
      )
    gdal.Unlink(asc_path)
    gdal.Unlink(filename)

    # Test that we can delete an existing tile.
    with self.driver.Create(filename, 256, 256, 1, gdal.GDT_UInt16) as ds:
      ds.SetGeoTransform([2, 0.001, 0, 49, 0, -0.001])
      sr = osr.SpatialReference()
      sr.ImportFromEPSG(4326)
      ds.SetProjection(sr.ExportToWkt())
      ds.GetRasterBand(1).SetNoDataValue(0)
      ds.GetRasterBand(1).Fill(1)
      ds.GetRasterBand(1).FlushCache()
      ds.GetRasterBand(1).Fill(0)
      ds.GetRasterBand(1).FlushCache()
      sql_lyr = ds.ExecuteSQL(
          'SELECT scale, offset FROM gpkg_2d_gridded_tile_ancillary'
      )
      self.assertIsNone(sql_lyr.GetNextFeature())
      ds.ReleaseResultSet(sql_lyr)
    gdal.Unlink(filename)

    # Test detecting tiles at zero (without nodata value).
    with self.driver.Create(filename, 256, 256, 1, gdal.GDT_Float32) as ds:
      ds.SetGeoTransform([2, 0.001, 0, 49, 0, -0.001])
      sr = osr.SpatialReference()
      sr.ImportFromEPSG(4326)
      ds.SetProjection(sr.ExportToWkt())
      ds.GetRasterBand(1).Fill(0)
      ds.GetRasterBand(1).FlushCache()
      sql_lyr = ds.ExecuteSQL('SELECT * FROM gpkg_39')
      self.assertIsNone(sql_lyr.GetNextFeature())
      ds.ReleaseResultSet(sql_lyr)
    gdal.Unlink(filename)

    # Test detecting tiles at nodata value.
    with self.driver.Create(filename, 256, 256, 1, gdal.GDT_Float32) as ds:
      ds.SetGeoTransform([2, 0.001, 0, 49, 0, -0.001])
      sr = osr.SpatialReference()
      sr.ImportFromEPSG(4326)
      ds.SetProjection(sr.ExportToWkt())
      ds.GetRasterBand(1).SetNoDataValue(2)
      ds.GetRasterBand(1).Fill(2)
      ds.GetRasterBand(1).FlushCache()
      sql_lyr = ds.ExecuteSQL('SELECT * FROM gpkg_39')
      self.assertIsNone(sql_lyr.GetNextFeature())
      ds.ReleaseResultSet(sql_lyr)

  @parameterized.parameters(['byte.tif', 'int16.tif'])
  def test_gpkg_statistics_stored_in_gpkg(self, base_filename: str):
    src_filename = pathlib.Path(
        gdrivers_util.GetTestFilePath(os.path.join('gtiff', base_filename))
    )
    src_ds = gdal.Open(src_filename)

    filename = pathlib.Path('/vsimem/gpkg_statistics_stored_in_gpkg.gpkg')
    with self.driver.CreateCopy(filename, src_ds) as ds:
      self.assertEqual(ds.GetFileList(), [str(filename)])
      ds.GetRasterBand(1).ComputeStatistics(False)
      self.assertEqual(
          ds.GetRasterBand(1).GetMetadata(),
          {
              'STATISTICS_MAXIMUM': '255',
              'STATISTICS_MEAN': '126.765',
              'STATISTICS_MINIMUM': '74',
              'STATISTICS_STDDEV': '22.928470838676',
              'STATISTICS_VALID_PERCENT': '100',
          },
      )

    aux_filename = str(filename) + '.aux.xml'
    self.assertIsNone(gdal.VSIStatL(aux_filename))

    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetFileList(), [str(filename)])
      self.assertEqual(
          ds.GetRasterBand(1).GetMetadata(),
          {
              'STATISTICS_MAXIMUM': '255',
              'STATISTICS_MEAN': '126.765',
              'STATISTICS_MINIMUM': '74',
              'STATISTICS_STDDEV': '22.928470838676',
              'STATISTICS_VALID_PERCENT': '100',
          },
      )
      self.assertEqual(
          set(ds.GetMetadataDomainList()),
          {'', 'IMAGE_STRUCTURE', 'DERIVED_SUBDATASETS'},
      )

    self.assertIsNone(gdal.VSIStatL(aux_filename))

    with gdal.Open(filename, gdal.GA_Update) as ds:
      ds.GetRasterBand(1).Fill(0)

    self.assertIsNone(gdal.VSIStatL(aux_filename))

    with gdal.Open(filename) as ds:
      self.assertEmpty(ds.GetRasterBand(1).GetMetadata())

    gdal.Unlink(filename)

  # test_gpkg_statistics_stored_in_pam - PAM_ENABLED is disabled in Google3.

  def test_gpkg_40(self):
    src_filename = pathlib.Path(gdrivers_util.GetTestFilePath('gtiff/byte.tif'))
    src_ds = gdal.Open(src_filename)
    filename = pathlib.Path('/vsimem/gpkg_40.gpkg')
    with gdal.Translate(filename, src_ds, format='GPKG') as ds:
      sql_lyr = ds.ExecuteSQL('PRAGMA application_id')
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['application_id'], 1196444487)
      ds.ReleaseResultSet(sql_lyr)
      sql_lyr = ds.ExecuteSQL('PRAGMA user_version')
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['user_version'], 10400)

      ds.ReleaseResultSet(sql_lyr)

    gdal.Unlink(filename)

  @parameterized.parameters([
      ['1.0', 1196437808, 0],
      ['1.1', 1196437809, 0],
      ['1.2', 1196444487, 10200],
  ])
  def test_gpkg_40_part2_(
      self, version: str, application_id: int, user_version: int
  ):
    src_filename = pathlib.Path(gdrivers_util.GetTestFilePath('gtiff/byte.tif'))
    src_ds = gdal.Open(src_filename)
    filename = pathlib.Path(f'/vsimem/gpkg_40_{version}.gpkg')

    with gdal.Translate(
        filename,
        src_ds,
        format='GPKG',
        creationOptions=[f'VERSION={version}'],
    ) as ds:
      sql_lyr = ds.ExecuteSQL('PRAGMA application_id')
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['application_id'], application_id)
      ds.ReleaseResultSet(sql_lyr)
      sql_lyr = ds.ExecuteSQL('PRAGMA user_version')
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['user_version'], user_version)

      ds.ReleaseResultSet(sql_lyr)

    gdal.Unlink(filename)

  # test_gpkg_41 - allocates 500M-pixel block; hits Forge/MSan memory limits.

  def test_gpkg_42(self):
    src_filename = pathlib.Path(self.getTestFilePath('byte.gpkg'))
    filename = pathlib.Path('/vsimem/gpkg_42.gpkg')
    with gdal.config_option('CREATE_GEOMETRY_COLUMNS', 'NO'):
      gdal.Translate(filename, src_filename, format='GPKG')

    with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'gpkg_geometry_columns'"
      )
      fc = sql_lyr.GetFeatureCount()
      ds.ReleaseResultSet(sql_lyr)
      self.assertEqual(fc, 0)
      self.assertIsNotNone(ds.CreateLayer('test'))
      ds.FlushCache()
      self.assertEmpty(gdal.GetLastErrorMsg())

    gdal.Unlink(filename)

  def test_gpkg_43(self):
    filename = pathlib.Path('/vsimem/gpkg_43.gpkg')
    with gdal.config_option('CREATE_RASTER_TABLES', 'NO'):
      ds = self.driver.Create(filename, 0, 0, 0, gdal.GDT_Unknown)
    ds.CreateLayer('foo')
    ds = None  # pylint: disable=unused-variable

    with gdal.OpenEx(filename, gdal.OF_UPDATE) as ds:
      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'gpkg_tile_matrix_set'"
      )
      fc = sql_lyr.GetFeatureCount()
      ds.ReleaseResultSet(sql_lyr)
      self.assertEqual(fc, 0)

    src_filename = pathlib.Path(self.getTestFilePath('byte.gpkg'))

    gdal.Translate(
        filename,
        src_filename,
        format=gdrivers_util.GPKG_DRIVER,
        creationOptions=['APPEND_SUBDATASET=YES'],
    )
    with gdal.OpenEx(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(ds.GetLayerCount(), 1)

    self.validate(filename)

    gdal.Unlink(filename)

  # test_gpkg_44 - ENABLE_SQL_GPKG_FORMAT not enabled.

  def test_gpkg_45(self):
    filename = pathlib.Path(self.getTestFilePath('byte.gpkg'))
    self.CheckOpen(str(filename))
    self.CheckBand(1, 4672, gdal.GDT_Byte)

  def test_gpkg_46(self):
    filename = str(self.vsimem_dir / 'gpkg_46.gpkg')
    with self.driver.Create(
        filename, 6698, 6698, options=['TILING_SCHEME=GoogleMapsCompatible']
    ) as ds:
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      ds.SetProjection(srs.ExportToWkt())
      ds.SetGeoTransform(
          [500, 0.037322767717371, 0, 750, 0, -0.037322767717371]
      )

    with gdal.Open(filename, gdal.GA_Update) as ds:
      ds.BuildOverviews('NEAR', [2, 4, 8, 16, 32, 64, 128, 256])

    with gdal.Open(filename) as ds:
      sql_lyr = ds.ExecuteSQL(
          'SELECT zoom_level, matrix_width * pixel_x_size * tile_width, '
          'matrix_height * pixel_y_size * tile_height '
          'FROM gpkg_tile_matrix ORDER BY zoom_level'
      )
      try:
        count = 0
        for f in sql_lyr:
          count += 1
          self.assertAlmostEqual(f.GetField(1), 40075016.6855785, delta=1e-7)
          self.assertAlmostEqual(f.GetField(2), 40075016.6855785, delta=1e-7)
      finally:
        ds.ReleaseResultSet(sql_lyr)

    self.assertEqual(count, 23)

  def test_gpkg_47(self):
    src_filename = pathlib.Path(gdrivers_util.GetTestFilePath('gtiff/byte.tif'))
    filename = pathlib.Path('/vsimem/gpkg_47.gpkg')
    with gdal.Open(src_filename) as src_ds:
      with self.driver.CreateCopy(filename, src_ds) as ds:
        ds.ExecuteSQL('UPDATE gpkg_contents SET min_x = 1, max_x = 0')

    with quiet_error_handler():
      with gdal.Open(filename) as ds:
        self.assertEqual(ds.RasterXSize, 256)

    gdal.Unlink(filename)

  def test_gpkg_48(self):
    byte_path = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    temp_dir = self.create_tempdir().full_path
    old_pwd = os.getcwd()
    try:
      os.chdir(temp_dir)
      filename = 'C:\\byte.gpkg'
      with gdal.Translate(
          filename,
          byte_path,
          format='GPKG',
          creationOptions=['RASTER_TABLE=foo'],
      ):
        pass
      with gdal.Translate(
          filename,
          byte_path,
          format='GPKG',
          creationOptions=['APPEND_SUBDATASET=YES', 'RASTER_TABLE=bar'],
      ):
        pass
      with gdal.Open(f'GPKG:{filename}:foo') as ds:
        self.assertIsNotNone(ds)
      with gdal.Open(f'GPKG:{filename}:bar') as ds:
        self.assertIsNotNone(ds)
      gdal.Unlink(filename)
    finally:
      os.chdir(old_pwd)

  def test_gpkg_delete_raster_layer(self):
    src_filename = pathlib.Path(gdrivers_util.GetTestFilePath('gtiff/byte.tif'))
    filename = pathlib.Path('/vsimem/delete_raster_layer.gpkg')
    gdal.Translate(
        filename,
        src_filename,
        format=gdrivers_util.GPKG_DRIVER,
        creationOptions=['RASTER_TABLE=foo'],
    )

    with ogr.Open(filename, update=1) as ds:
      ds.ExecuteSQL('DROP TABLE foo')
      sql_lyr = ds.ExecuteSQL('SELECT * FROM gpkg_metadata')
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      ds.ReleaseResultSet(sql_lyr)
      ds.ExecuteSQL('VACUUM')

    # Check that there is no more any reference to the layer
    f = gdal.VSIFOpenL(filename, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn('foo', content)

    gdal.Unlink(filename)

  def test_gpkg_delete_gridded_coverage_raster_layer(self):
    src_filename = pathlib.Path(
        gdrivers_util.GetTestFilePath('gtiff/float32.tif')
    )
    filename = pathlib.Path('/vsimem/delete_gridded_coverage_raster_layer.gpkg')
    gdal.Translate(
        filename,
        src_filename,
        format=gdrivers_util.GPKG_DRIVER,
        creationOptions=['RASTER_TABLE=foo'],
    )

    with ogr.Open(filename, update=1) as ds:
      ds.ExecuteSQL('DROP TABLE foo')
      ds.ExecuteSQL('VACUUM')

    # Check that there is no more any reference to the layer
    f = gdal.VSIFOpenL(filename, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn('foo', content)

    gdal.Unlink(filename)

  def test_gpkg_open_old_gpkg_elevation_tiles_extension(self):
    filepath = self.getTestFilePath('uint16-old-elevation-extension.gpkg')
    self.CheckOpen(filepath)
    self.CheckBand(1, 4672, gdal.GDT_UInt16, None)

  def test_gpkg_GeneralCmdLineProcessor(self):
    srcdir = googletest.get_default_test_srcdir()
    gdalinfo_path = os.path.join(srcdir, 'google3/third_party/gdal/gdalinfo')
    ogrinfo_path = os.path.join(srcdir, 'google3/third_party/gdal/ogrinfo')

    ret_gdalinfo = subprocess.check_output(
        [gdalinfo_path, '--format', 'GPKG']
    ).decode('utf-8')
    ret_ogrinfo = subprocess.check_output(
        [ogrinfo_path, '--format', 'GPKG']
    ).decode('utf-8')

    self.assertIn('<CreationOptionList>', ret_gdalinfo)
    self.assertIn('<CreationOptionList>', ret_ogrinfo)
    self.assertNotIn('scope=', ret_gdalinfo)
    self.assertNotIn('scope=', ret_ogrinfo)

  def test_gpkg_match_overview_factor(self):
    filename = pathlib.Path('/vsimem/gpkg_match_overview_factor.gpkg')
    src_filename = pathlib.Path(
        self.getTestFilePath('test_match_overview_factor.gpkg')
    )

    gdal.FileFromMemBuffer(filename, src_filename.read_bytes())

    with gdal.Open(filename, gdal.GA_Update) as ds:
      ret = ds.BuildOverviews(
          'NONE', [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]
      )
      self.assertEqual(ret, 0)
      self.assertEmpty(gdal.GetLastErrorMsg())

    gdal.Unlink(filename)

  @parameterized.parameters(['1.2', '1.4'])
  def test_gpkg_wkt2(self, version: str):
    filename = pathlib.Path(f'/vsimem/wkt2_{version}.gpkg')
    with self.driver.Create(
        filename, 1, 1, options=[f'VERSION={version}']
    ) as ds:
      sr = osr.SpatialReference()
      sr.ImportFromEPSG(4979)  # WGS 84 3D
      sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
      ds.SetSpatialRef(sr)
      ds.SetGeoTransform([2, 1, 0, 49, 0, -1])

    with gdal.Open(filename) as ds:
      sr_got = ds.GetSpatialRef()
      self.assertTrue(sr_got.IsSame(sr))

      lyr = ds.ExecuteSQL('SELECT * FROM gpkg_spatial_ref_sys ORDER BY srs_id')
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetField('srs_name'), 'Undefined Cartesian SRS')
      self.assertEqual(f.GetField('srs_id'), -1)
      self.assertEqual(f.GetField('organization'), 'NONE')
      self.assertEqual(f.GetField('organization_coordsys_id'), -1)
      self.assertEqual(f.GetField('definition'), 'undefined')
      self.assertEqual(
          f.GetField('description'),
          'undefined Cartesian coordinate reference system',
      )
      self.assertEqual(f.GetField('definition_12_063'), 'undefined')

      lyr.GetNextFeature()

      f = lyr.GetNextFeature()
      self.assertTrue(f.GetField('definition').startswith('GEOGCS["WGS 84"'))
      self.assertTrue(
          f.GetField('definition_12_063').startswith('GEODCRS["WGS 84"')
      )
      self.assertIn('ID["EPSG",4326]', f.GetField('definition_12_063'))

      f = lyr.GetNextFeature()
      self.assertEqual(f.GetField('definition'), 'undefined')
      self.assertTrue(
          f.GetField('definition_12_063').startswith('GEODCRS["WGS 84"')
      )
      self.assertIn('ID["EPSG",4979]', f.GetField('definition_12_063'))

      ds.ReleaseResultSet(lyr)

      lyr = ds.ExecuteSQL(
          "SELECT * FROM gpkg_extensions WHERE extension_name = 'gpkg_crs_wkt'"
      )
      self.assertEqual(lyr.GetFeatureCount(), 1 if version == '1.2' else 0)
      ds.ReleaseResultSet(lyr)

    # WKT2-only compatible SRS without EPSG code
    with self.driver.Create(
        filename,
        1,
        1,
        options=['APPEND_SUBDATASET=YES', 'RASTER_TABLE=table2'],
    ) as ds:
      sr.SetFromUserInput(
          'GEODCRS["my CRS",DATUM["my datum",'
          'ELLIPSOID["WGS 84",6378137,298.257223563,LENGTHUNIT["metre",1]]],'
          'PRIMEM["Greenwich",0,ANGLEUNIT["degree",0.0174532925199433]],'
          'CS[ellipsoidal,3],AXIS["geodetic latitude (Lat)",north,ORDER[1],'
          'ANGLEUNIT["degree",0.0174532925199433]],'
          'AXIS["geodetic longitude (Lon)",east,ORDER[2],'
          'ANGLEUNIT["degree",0.0174532925199433]],'
          'AXIS["ellipsoidal height (h)",up,ORDER[3],LENGTHUNIT["metre",1]]]'
      )
      sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
      ds.SetSpatialRef(sr)
      ds.SetGeoTransform([2, 1, 0, 49, 0, -1])

    with gdal.Open(f'GPKG:{filename}:table2') as ds:
      sr_got = ds.GetSpatialRef()
      self.assertTrue(sr_got.IsSame(sr))

    # WKT1 compatible SRS
    with self.driver.Create(
        filename, 1, 1, options=['APPEND_SUBDATASET=YES', 'RASTER_TABLE=table3']
    ) as ds:
      sr.ImportFromEPSG(32631)
      sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
      ds.SetSpatialRef(sr)
      ds.SetGeoTransform([500000, 1, 0, 4500000, 0, -1])

    with gdal.Open(f'GPKG:{filename}:table3') as ds:
      sr_got = ds.GetSpatialRef()
      self.assertTrue(sr_got.IsSame(sr))

      lyr = ds.ExecuteSQL(
          'SELECT * FROM gpkg_spatial_ref_sys WHERE srs_id = 32631'
      )
      f = lyr.GetNextFeature()
      self.assertTrue(
          f.GetField('definition').startswith('PROJCS["WGS 84 / UTM zone 31N",')
      )
      self.assertIn('AUTHORITY["EPSG","32631"]', f.GetField('definition'))
      self.assertTrue(
          f.GetField('definition_12_063').startswith(
              'PROJCRS["WGS 84 / UTM zone 31N",'
          )
      )
      self.assertIn('ID["EPSG",32631]', f.GetField('definition_12_063'))
      ds.ReleaseResultSet(lyr)

    self.validate(filename)

    gdal.Unlink(filename)

  # test_gpkg_50000_25000_uint16 - slow 2.5GB+ memory, unsuitable for Forge.
  # test_gpkg_50000_50000_uint16 - slow 5GB+ memory, unsuitable for Forge.

  def test_gpkg_float32_png_negative_values(self):
    filename = pathlib.Path('/vsimem/float32_png_negative_values.gpkg')
    with self.driver.Create(
        filename, 1, 1, 1, gdal.GDT_Float32, options=['TILE_FORMAT=PNG']
    ) as ds:
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      ds.GetRasterBand(1).SetNoDataValue(-32768)
      ds.GetRasterBand(1).Fill(-10)

    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).ComputeRasterMinMax(), (-10, -10))

  def test_gpkg_coordinate_epoch(self):
    filename = pathlib.Path('/vsimem/coordinate_epoch.gpkg')
    with self.driver.Create(filename, 1, 1) as ds:
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      srs.SetCoordinateEpoch(2021.3)
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      ds.SetSpatialRef(srs)

    with gdal.Open(filename) as ds:
      srs = ds.GetSpatialRef()
      self.assertEqual(srs.GetCoordinateEpoch(), 2021.3)

    self.driver.Delete(filename)

  def test_gpkg_coordinate_epoch_is_dynamic(self):
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with self.driver.Create(filename, 1, 1) as ds:
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(9000)
      self.assertTrue(srs.IsDynamic())
      srs.SetCoordinateEpoch(2021.3)
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      ds.SetSpatialRef(srs)

    with gdal.Open(filename) as ds:
      srs = ds.GetSpatialRef()
      self.assertTrue(srs.IsDynamic())
      self.assertEqual(srs.GetCoordinateEpoch(), 2021.3)

  @parameterized.parameters(['PNG_JPEG', 'PNG', 'PNG8', 'JPEG', 'WEBP'])
  def test_gpkg_flushing_not_all_bands(self, tile_format: str):
    out_filename = str(self.vsimem_dir / 'test.gpkg')
    with self.driver.Create(
        out_filename, 256, 256, 4, options=[f'TILE_FORMAT={tile_format}']
    ) as ds:
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      ds.GetRasterBand(1).Fill(255)
      ds.GetRasterBand(2).Fill(255)
      ds.GetRasterBand(3).Fill(255)
      ds.FlushCache()
      ds.GetRasterBand(4).Fill(255)

    with gdal.Open(out_filename) as ds:
      self.assertEqual(
          [
              ds.GetRasterBand(i + 1).ComputeRasterMinMax()
              for i in range(ds.RasterCount)
          ],
          [(255.0, 255.0)] * 4,
      )

  def test_gpkg_uint16_tiling_scheme_nodata_overview(self):
    with gdal.GetDriverByName('MEM').Create(
        '', 100, 100, 1, gdal.GDT_UInt16
    ) as src_ds:
      src_ds.GetRasterBand(1).SetNoDataValue(-9999)
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      src_ds.SetProjection(srs.ExportToWkt())
      src_ds.SetGeoTransform([
          -180,
          src_ds.RasterXSize / 360.0,
          0,
          90,
          0,
          -src_ds.RasterYSize / 180.0,
      ])

      out_filename = str(self.vsimem_dir / 'test.gpkg')
      with self.driver.CreateCopy(
          out_filename, src_ds, options=['TILING_SCHEME=GoogleCRS84Quad']
      ) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetNoDataValue(),
            src_ds.GetRasterBand(1).GetNoDataValue(),
        )
        self.assertEqual(
            ds.GetRasterBand(1).GetOverview(0).GetNoDataValue(),
            src_ds.GetRasterBand(1).GetNoDataValue(),
        )

  @parameterized.parameters([1, 2])
  def test_gpkg_byte_nodata_value(self, band_count: int):
    filename = pathlib.Path(f'/vsimem/byte_nodata_value_{band_count}.gpkg')

    with self.driver.Create(
        filename, 1, 1, band_count, gdal.GDT_Byte, options=['TILE_FORMAT=PNG']
    ) as ds:
      ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      message = 'Invalid nodata value for a Byte band: -32768'
      with self.assertRaisesRegex(RuntimeError, message):
        ds.GetRasterBand(1).SetNoDataValue(-32768)
      self.assertEqual(ds.GetRasterBand(1).SetNoDataValue(255), gdal.CE_None)
      if band_count == 2:
        message = 'Only the same nodata value can be set on all bands'
        with self.assertRaisesRegex(RuntimeError, message):
          ds.GetRasterBand(2).SetNoDataValue(254)

    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 255)
      if band_count == 2:
        self.assertEqual(ds.GetRasterBand(2).GetNoDataValue(), 255)

  @parameterized.parameters([
      ('XXXXXXXXX:/test.gpkg:layer1', '', ''),
      ('GPKG:/test.gpkg', '', ''),
      ('GPKG:/test.gpkg:layer1', '/test.gpkg', 'layer1'),
      ('gpkg:/test.gpkg:layer1', '/test.gpkg', 'layer1'),
      (r'GPKG:c:\test.gpkg', '', ''),
      (r'GPKG:c:\test.gpkg:layer1', r'c:\test.gpkg', 'layer1'),
      (r'gpkg:c:\test.gpkg:layer1', r'c:\test.gpkg', 'layer1'),
  ])
  def test_gpkg_gdal_subdataset_get_filename(
      self,
      filename: str,
      path_component: str,
      subdataset_component: str,
  ):
    info = gdal.GetSubdatasetInfo(filename)
    if not path_component:
      self.assertIsNone(info)
    else:
      self.assertEqual(info.GetPathComponent(), path_component)
      self.assertEqual(info.GetSubdatasetComponent(), subdataset_component)

  @parameterized.parameters([
      ('GPKG:/test.gpkg', ''),
      ('GPKG:/test.gpkg:layer1', 'GPKG:/new/test.gpkg:layer1'),
      ('gpkg:/test.gpkg:layer1', 'gpkg:/new/test.gpkg:layer1'),
      (r'GPKG:c:\test.gpkg:layer1', 'GPKG:/new/test.gpkg:layer1'),
      (r'gpkg:c:\test.gpkg:layer1', 'gpkg:/new/test.gpkg:layer1'),
  ])
  def test_gpkg_gdal_subdataset_modify_filename(
      self, subdataset_component: str, new_path: str
  ):
    info = gdal.GetSubdatasetInfo(subdataset_component)
    if not new_path:
      self.assertIsNone(info)
    else:
      self.assertEqual(info.ModifyPathComponent('/new/test.gpkg'), new_path)

  def test_gpkg_sql_gdal_get_layer_pixel_value(self):
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_sql_gdal_get_layer_pixel_value.gpkg'
    )
    with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
      with self.driver.CreateCopy(
          filename, src_ds, options=['RASTER_TABLE=byte']
      ):
        pass
    with gdal.Open(
        gdrivers_util.GetTestFilePath('gtiff/float32.tif')
    ) as src_ds:
      with self.driver.CreateCopy(
          filename, src_ds, options=['APPEND_SUBDATASET=YES']
      ):
        pass

    with gdal.OpenEx(filename) as ds:
      sql_lyr = ds.ExecuteSQL(
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'georef', "
          '440780, 3751080)'
      )
      self.assertEqual(sql_lyr.GetNextFeature()[0], 156)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'pixel', 1, 4)"
      )
      self.assertEqual(sql_lyr.GetNextFeature()[0], 156)
      ds.ReleaseResultSet(sql_lyr)

      with ds.ExecuteSQL(
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'georef', "
          '440780 + 30, 3751080 - 30)'
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetNextFeature()[0], 156)

      with ds.ExecuteSQL(
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'georef', "
          "440780 + 30, 3751080 - 30, 'cubicspline')"
      ) as sql_lyr:
        self.assertAlmostEqual(sql_lyr.GetNextFeature()[0], 150.1388888888889)

      sql_lyr = ds.ExecuteSQL(
          "SELECT gdal_get_layer_pixel_value('float32', 1, 'pixel', 0, 1)"
      )
      self.assertEqual(sql_lyr.GetNextFeature()[0], 115.0)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'pixel', -1, 0)"
      )
      self.assertIsNone(sql_lyr.GetNextFeature()[0])
      ds.ReleaseResultSet(sql_lyr)

      bad_queries = [
          "SELECT gdal_get_layer_pixel_value(NULL, 1, 'pixel', 0, 0)",
          "SELECT gdal_get_layer_pixel_value('byte', NULL, 'pixel', 0, 0)",
          "SELECT gdal_get_layer_pixel_value('byte', 1, NULL, 0, 0)",
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'pixel', NULL, 0)",
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'pixel', 0, NULL)",
          "SELECT gdal_get_layer_pixel_value('byte', 0, 'pixel', 0, 0)",
          "SELECT gdal_get_layer_pixel_value('byte', 1, 'invalid', 0, 0)",
      ]
      for query in bad_queries:
        with no_gdal_exceptions(), quiet_error_handler():
          sql_lyr = ds.ExecuteSQL(query)
          feat = sql_lyr.GetNextFeature()
          self.assertIsNone(feat[0], msg=query)
          ds.ReleaseResultSet(sql_lyr)

  def test_gpkg_gti_gpkg_ext(self):
    filename = str(self.vsimem_dir / 'test_gpkg_gti_gpkg_ext.gti.gpkg')
    with gdal.Translate(
        filename, gdrivers_util.GetTestFilePath('gtiff/byte.tif'), format='GPKG'
    ):
      pass
    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetDriver().ShortName, 'GPKG')
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  @parameterized.parameters([gdal.GDT_Byte, gdal.GDT_UInt16])
  def test_gpkg_rename_raster_table(self, data_type: int):
    test_layer_path = str(
        self.vsimem_dir / 'test_gpkg_rename_raster_table.gpkg'
    )
    src_name = (
        'int16.tif' if data_type == gdal.GDT_UInt16 else 'small_world.tif'
    )
    with gdal.Open(
        gdrivers_util.GetTestFilePath(f'gtiff/{src_name}')
    ) as src_ds:
      with self.driver.CreateCopy(
          test_layer_path,
          src_ds,
          options=['TILE_FORMAT=PNG', 'RASTER_TABLE=weird\'layer"name'],
      ):
        pass

    with gdal.OpenEx(test_layer_path, gdal.OF_RASTER | gdal.OF_UPDATE) as ds:
      self.assertEqual(ds.GetMetadataItem('IDENTIFIER'), 'weird\'layer"name')
      checksum = ds.GetRasterBand(1).Checksum()
      ds.ExecuteSQL('ALTER TABLE "weird\'layer""name" RENAME TO bar')
      ds.ExecuteSQL('VACUUM')

    with gdal.Open(test_layer_path) as ds:
      self.assertEqual(ds.GetMetadataItem('IDENTIFIER'), 'bar')
      self.assertEqual(ds.GetRasterBand(1).Checksum(), checksum)

    content = gcore_util.vsi_read_file_as_bytes(
        test_layer_path, 1_000_000
    ).decode('latin1')
    self.assertNotIn('weird', content)

  def test_gpkg_copy_using_get_data_coverage_status(self):
    tmp_gtiff = str(self.vsimem_dir / 'tmp.tif')
    with gdal.GetDriverByName('GTiff').Create(
        tmp_gtiff,
        1024,
        768,
        1,
        options=[
            'TILED=YES',
            'BLOCKXSIZE=256',
            'BLOCKYSIZE=256',
            'SPARSE_OK=YES',
        ],
    ) as src_ds:
      src_ds.SetGeoTransform([2, 0.001, 0, 49, 0, -0.001])
      src_ds.WriteRaster(512, 256, 256, 256, b'\x01' * (256 * 256))
      src_checksum = src_ds.GetRasterBand(1).Checksum()

      tmp_gpkg = str(self.vsimem_dir / 'tmp.gpkg')
      with self.driver.CreateCopy(tmp_gpkg, src_ds):
        pass

    with gdal.Open(tmp_gpkg) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), src_checksum)

      with ds.ExecuteSQL('SELECT COUNT(*) FROM tmp') as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)

      both_flags = (
          gdal.GDAL_DATA_COVERAGE_STATUS_DATA
          | gdal.GDAL_DATA_COVERAGE_STATUS_EMPTY
      )
      band = ds.GetRasterBand(1)

      flags, pct = band.GetDataCoverageStatus(0, 0, 1024, 768)
      self.assertEqual(flags, both_flags)
      self.assertAlmostEqual(pct, 100.0 / 12)

      for win in (
          (0, 0, 1024, 256),
          (0, 512, 1024, 256),
          (0, 0, 512, 768),
          (768, 0, 256, 768),
      ):
        flags, pct = band.GetDataCoverageStatus(*win)
        self.assertEqual(flags, gdal.GDAL_DATA_COVERAGE_STATUS_EMPTY)
        self.assertEqual(pct, 0.0)

      for win in ((512, 256, 256, 256), (513, 258, 3, 4)):
        flags, pct = band.GetDataCoverageStatus(*win)
        self.assertEqual(flags, gdal.GDAL_DATA_COVERAGE_STATUS_DATA)
        self.assertEqual(pct, 100.0)

      flags, pct = band.GetDataCoverageStatus(511, 255, 2, 2)
      self.assertEqual(flags, both_flags)
      self.assertEqual(pct, 25.0)


if __name__ == '__main__':
  googletest.main()
