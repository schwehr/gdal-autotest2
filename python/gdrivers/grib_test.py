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
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2008, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at mines-paris dot org>
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

"""Tests for the GRIB driver.

Format is described here:

https://gdal.org/drivers/raster/grib.html

Rewrite of grib.py:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py
"""

import glob
import os
import shutil
import struct
from typing import Any

from osgeo import gdal
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.GRIB_DRIVER
EXT = '.grb'


def _has_jp2k_driver() -> bool:
  for i in range(gdal.GetDriverCount()):
    if gdal.GetDriver(i).ShortName.startswith('JP2'):
      return True
  return False


def _grib_read_section(filename: str, sect_num_to_read: int) -> bytes | None:
  fh = gdal.VSIFOpenL(filename, 'rb')
  if fh is None:
    return None
  try:
    # Ignore Sect 0.
    gdal.VSIFReadL(1, 16, fh)
    while True:
      sect_size_bytes = gdal.VSIFReadL(1, 4, fh)
      if not sect_size_bytes:
        return None
      sect_size_num = struct.unpack('>I', sect_size_bytes)[0]
      sect_num_bytes = gdal.VSIFReadL(1, 1, fh)
      sect_num = ord(sect_num_bytes)
      if sect_num == sect_num_to_read:
        return (
            sect_size_bytes
            + sect_num_bytes
            + gdal.VSIFReadL(1, sect_size_num - 5, fh)
        )
      gdal.VSIFSeekL(fh, sect_size_num - 5, 1)
  finally:
    gdal.VSIFCloseL(fh)


class GribTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    gdal.DontUseExceptions()

  def getTestFilePath(self, filename: str) -> str:
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def _assertBandMetadataSubset(
      self, band: gdal.Band, expected_md: dict[str, str]
  ) -> None:
    md = band.GetMetadata()
    for k, v in expected_md.items():
      self.assertIn(k, md, msg=f'Missing metadata key {k}')
      self.assertEqual(md[k], v, msg=f'Metadata mismatch for key {k}')

  def _assertGeoTransformAlmostEqual(
      self,
      got_gt: tuple[float, ...],
      expected_gt: tuple[float, ...],
      delta: float | None = None,
      rel: float | None = None,
  ) -> None:
    self.assertLen(got_gt, 6)
    self.assertLen(expected_gt, 6)
    for idx, (got, expected) in enumerate(zip(got_gt, expected_gt)):
      if rel is not None:
        tol = max(abs(expected) * rel, 1e-12)
        self.assertAlmostEqual(
            got, expected, delta=tol, msg=f'GeoTransform[{idx}] differs'
        )
      elif delta is not None:
        self.assertAlmostEqual(
            got, expected, delta=delta, msg=f'GeoTransform[{idx}] differs'
        )
      else:
        self.assertAlmostEqual(
            got, expected, msg=f'GeoTransform[{idx}] differs'
        )

  def _createMemWgs84Dataset(
      self,
      width: int,
      height: int,
      datatype: int = gdal.GDT_Byte,
      gt: tuple[float, ...] = (2.0, 1.0, 0.0, 49.0, 0.0, -1.0),
  ) -> gdal.Dataset:
    ds = gdal.GetDriverByName('MEM').Create('', width, height, 1, datatype)
    self.assertIsNotNone(ds)
    assert ds is not None
    ds.SetGeoTransform(gt)
    sr = osr.SpatialReference()
    sr.SetFromUserInput('WGS84')
    ds.SetProjection(sr.ExportToWkt())
    return ds

  def _assertWriteRotatedLatLon(self, name: str, srs_input: str) -> None:
    filename = f'/vsimem/{name}.grb2'
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    srs = osr.SpatialReference()
    srs.SetFromUserInput(srs_input)
    src_ds.SetSpatialRef(srs)
    src_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])

    with gcore_util.GdalUnlinkWhenDone(filename):
      out_ds = gdal.Translate(filename, src_ds, format='GRIB')
      self.assertIsNotNone(out_ds)
      out_ds = None
      self.assertIsNone(gdal.VSIStatL(filename + '.aux.xml'))
      with gdal.Open(filename) as ds:
        self.assertIsNotNone(ds)
        assert ds is not None
        self.assertEqual(
            ds.GetSpatialRef().ExportToProj4(),
            '+proj=ob_tran +o_proj=longlat +o_lon_p=-10 +o_lat_p=39.25 '
            '+lon_0=18 +a=6367470 +rf=594.313048347956 +no_defs',
        )
        self._assertGeoTransformAlmostEqual(
            ds.GetGeoTransform(), (2.0, 1.0, 0.0, 49.0, 0.0, -1.0)
        )

  def _assertSidecarComparison(
      self,
      filepath: str,
      expected_count: int,
      band6_desc_idx: str,
      band6_desc_no_idx: str,
  ) -> None:
    ds_idx = gdal.Open(filepath)
    self.assertIsNotNone(ds_idx)
    assert ds_idx is not None
    self.assertEqual(ds_idx.RasterCount, expected_count)
    self.assertEqual(
        ds_idx.GetRasterBand(6).GetDescription(),
        band6_desc_idx,
        'Description does not match, sidecar index is probably ignored',
    )

    ds_no_idx = gdal.OpenEx(
        filepath, gdal.GA_ReadOnly, open_options=['USE_IDX=NO']
    )
    self.assertIsNotNone(ds_no_idx)
    assert ds_no_idx is not None
    self.assertEqual(ds_no_idx.RasterCount, ds_idx.RasterCount)
    self.assertEqual(
        ds_no_idx.GetRasterBand(6).GetDescription(),
        band6_desc_no_idx,
        'Description does not match, sidecar index is probably loaded',
    )

    for i in range(1, ds_no_idx.RasterCount + 1):
      b_no_idx = ds_no_idx.GetRasterBand(i)
      b_idx = ds_idx.GetRasterBand(i)
      self.assertEqual(b_no_idx.Checksum(), b_idx.Checksum())
      md_no_idx = b_no_idx.GetMetadata()
      md_idx = b_idx.GetMetadata()
      self.assertEqual(md_no_idx.keys(), md_idx.keys())
      for key in md_no_idx:
        self.assertEqual(
            b_no_idx.GetMetadataItem(key), b_idx.GetMetadataItem(key)
        )

  def testOpen(self):
    # Upstream grib_1.
    self.CheckDriver()
    filepath = self.getTestFilePath('ds.mint.bin')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((-7127137, 2500, 0, 2105561, 0, -2500))
    wkt = """PROJCS["unnamed",
    GEOGCS["Coordinate System imported from GRIB file",
        DATUM["unnamed",
              SPHEROID["Sphere",6371200,0]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.0174532925199433]],
    PROJECTION["Mercator_2SP"],
    PARAMETER["standard_parallel_1",20],
    PARAMETER["central_meridian",0],
    PARAMETER["false_easting",0],
    PARAMETER["false_northing",0],UNIT["m",1]]"""
    self.CheckProjection(wkt)
    self.CheckShape(177, 129, 2)
    self.CheckBand(1, 46650, gdal.GDT_Float64, 9999, 13.24999390, 24.9500061035)
    self.CheckBand(2, 46927, gdal.GDT_Float64, 9999, 14.95000610, 24.9500061035)

  def testGrib1(self):
    # Upstream grib_2.
    filepath = self.getTestFilePath('Sample_QuikSCAT.grb')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((-20.2465, 0.333, 0, 56.0505, 0, -0.333))
    wkt = """GEOGCS["Coordinate System imported from GRIB file",
    DATUM["unnamed",
        SPHEROID["Sphere",6371200,0]],
    PRIMEM["Greenwich",0],
    UNIT["degree",0.0174532925199433]]"""
    self.CheckProjection(wkt)
    self.CheckShape(66, 74, 4)
    self.CheckBand(1, 35740, gdal.GDT_Float64, 9999, 0, 1)
    self.CheckBand(2, 24744, gdal.GDT_Float64, 9999, -16.415, 6.299)
    self.CheckBand(3, 40666, gdal.GDT_Float64, 9999, -11.679, 13.66)
    self.CheckBand(4, 50714, gdal.GDT_Float64, 9.999e20, 19598, 25986)
    # Call GetNoDataValue twice to test caching.
    ds = self.src
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 9999.0)
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 9999.0)

  def test_grib_read_different_sizes_messages(self):
    filepath = self.getTestFilePath('bug3246.grb')
    with gdal.quiet_errors():
      self.CheckOpen(filepath)
      self.CheckBand(4, 4081)
    msg = gdal.GetLastErrorMsg()
    self.assertIn('data access may be incomplete', msg)
    self.assertEqual(gdal.GetLastErrorType(), 2)

  def test_grib_grib2_read_nodata(self):
    ds = gdal.Open(self.getTestFilePath('ds.mint.bin'))
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 9999)
    self.assertEqual(ds.GetRasterBand(2).GetNoDataValue(), 9999)
    expected_md = {
        'GRIB_REF_TIME': '1203613200',
        'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
            '0 5 2 0 0 255 255 1 19 1 0 0 255 -1 -2147483647 2008 2 22 12 0 0'
            ' 1 0 3 255 1 12 1 0'
        ),
        'GRIB_VALID_TIME': '1203681600',
        'GRIB_FORECAST_SECONDS': '68400',
        'GRIB_UNIT': '[C]',
        'GRIB_PDS_TEMPLATE_NUMBERS': (
            '0 5 2 0 0 0 255 255 1 0 0 0 19 1 0 0 0 0 0 255 129 255 255 255 255'
            ' 7 216 2 22 12 0 0 1 0 0 0 0 3 255 1 0 0 0 12 1 0 0 0 0'
        ),
        'GRIB_DISCIPLINE': '0(Meteorological)',
        'GRIB_PDS_PDTN': '8',
        'GRIB_COMMENT': 'Minimum temperature [C]',
        'GRIB_SHORT_NAME': '0-SFC',
        'GRIB_ELEMENT': 'MinT',
    }
    self._assertBandMetadataSubset(ds.GetRasterBand(1), expected_md)

  @parameterized.parameters([
      (1, False),
      (1, True),
      (2, False),
      (2, True),
  ])
  def test_grib_grib2_read_nodata_bands_with_bitmap(
      self, band_nr: int, call_getmetadata_before: bool
  ):
    ds = gdal.Open(self.getTestFilePath('two_bands_with_bitmap.grib2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    if call_getmetadata_before:
      ds.GetRasterBand(band_nr).GetMetadata()
    self.assertEqual(ds.GetRasterBand(band_nr).GetNoDataValue(), 9999)

  def test_grib_read_units(self):
    temp_dir = self.create_tempdir().full_path
    tmp_path = os.path.join(temp_dir, 'ds.mint.bin')
    shutil.copy(self.getTestFilePath('ds.mint.bin'), tmp_path)

    ds = gdal.Open(tmp_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    md = ds.GetRasterBand(1).GetMetadata()
    self.assertEqual(md['GRIB_UNIT'], '[C]')
    self.assertEqual(md['GRIB_COMMENT'], 'Minimum temperature [C]')
    ds.GetRasterBand(1).ComputeStatistics(False)
    self.assertAlmostEqual(ds.GetRasterBand(1).GetMinimum(), 13, delta=1)
    ds = None

    gdal.Unlink(tmp_path + '.aux.xml')

    with gdaltest.config_option('GRIB_NORMALIZE_UNITS', 'NO'):
      ds = gdal.Open(tmp_path)
      self.assertIsNotNone(ds)
      assert ds is not None
      ds.GetRasterBand(1).ComputeStatistics(False)
      md = ds.GetRasterBand(1).GetMetadata()
      self.assertEqual(md['GRIB_UNIT'], '[K]')
      self.assertEqual(md['GRIB_COMMENT'], 'Minimum temperature [K]')
      self.assertAlmostEqual(ds.GetRasterBand(1).GetMinimum(), 286, delta=1)
      ds = None

    self.driver.Delete(tmp_path)

  def test_grib_read_geotransform_one_n_or_n_one(self):
    ds = gdal.Open(self.getTestFilePath('one_one.grib2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(
        ds.GetGeoTransform(), (-114.25, 0.5, 0.0, 47.250, 0.0, -0.5)
    )

  def test_grib_read_vsizip(self):
    zip_path = self.getTestFilePath('gfs.t00z.mastergrb2f03.zip')
    ds = gdal.Open(f'/vsizip/{zip_path}/gfs.t00z.mastergrb2f03')
    self.assertIsNotNone(ds)

  def test_grib_grib2_test_grib_pds_all_bands(self):
    zip_path = self.getTestFilePath('gfs.t00z.mastergrb2f03.zip')
    vsi_path = f'/vsizip/{zip_path}/gfs.t00z.mastergrb2f03'

    ds = gdal.Open(vsi_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertIsNotNone(
        ds.GetRasterBand(2).GetMetadataItem('GRIB_PDS_TEMPLATE_NUMBERS')
    )
    ds = None

    with gdaltest.config_option('GRIB_PDS_ALL_BANDS', 'OFF'):
      ds = gdal.Open(vsi_path)
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertIsNone(
          ds.GetRasterBand(2).GetMetadataItem('GRIB_PDS_TEMPLATE_NUMBERS')
      )
      ds = None

  def test_grib_grib2_read_template_4_15(self):
    filepath = self.getTestFilePath('template_4_15.grb2')
    with gdal.quiet_errors():
      ds = gdal.Open(filepath)
      self.assertIsNotNone(ds)
      assert ds is not None
      cs = ds.GetRasterBand(1).Checksum()
    if _has_jp2k_driver():
      self.assertGreaterEqual(cs, 0)
    else:
      self.assertIn('Is the JPEG2000 driver available?', gdal.GetLastErrorMsg())

  def test_grib_grib2_read_png(self):
    if gdal.GetDriverByName('PNG') is None:
      self.skipTest('PNG driver not available')
    ds = gdal.Open(
        self.getTestFilePath('MRMS_EchoTop_18_00.50_20161015-133230.grib2')
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    # google3 builds g2clib without USE_PNG, so Template 5.41 metadata loads
    # while decoding pixel data fails (upstream expects checksum 41854).
    with gdal.quiet_errors():
      self.assertEqual(ds.GetRasterBand(1).Checksum(), -1)

  def test_grib_grib2_read_template_4_32(self):
    ds = gdal.Open(
        self.getTestFilePath(
            'twenty-se27w.2017102006.hwrfsat.core.0p02.f000_truncated.grb2'
        )
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    band = ds.GetRasterBand(1)
    self.assertEqual(band.Checksum(), 48230)
    minmax = band.ComputeRasterMinMax()
    self.assertAlmostEqual(minmax[0], -9.765, delta=1e-2)
    self.assertAlmostEqual(minmax[1], 2.415, delta=1e-2)
    expected_md = {
        'GRIB_REF_TIME': '1508479200',
        'GRIB_VALID_TIME': '1508479200',
        'GRIB_FORECAST_SECONDS': '0',
        'GRIB_UNIT': '[C]',
        'GRIB_PDS_TEMPLATE_NUMBERS': (
            '5 7 2 0 0 0 0 0 1 0 0 0 0 1 0 31 1 29 67 140 2 0 0 238 217'
        ),
        'GRIB_PDS_PDTN': '32',
        'GRIB_COMMENT': 'Brightness Temperature [C]',
        'GRIB_SHORT_NAME': '0 undefined',
        'GRIB_ELEMENT': 'BRTEMP',
        'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
            '5 7 2 0 0 0 0 1 0 1 31 285 17292 2 61145'
        ),
    }
    self._assertBandMetadataSubset(band, expected_md)

  def test_grib_grib2_read_all_zero_data(self):
    ds = gdal.Open(
        self.getTestFilePath(
            'CMC_rdwps_lake-erie_ICEC_SFC_0_latlon0.05x0.05_2017111800_P000.grib2'
        )
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    band = ds.GetRasterBand(1)
    self.assertEqual(band.Checksum(), 0)
    expected_md = {
        'GRIB_REF_TIME': '1510963200',
        'GRIB_VALID_TIME': '1510963200',
        'GRIB_FORECAST_SECONDS': '0',
        'GRIB_UNIT': '[Proportion]',
        'GRIB_PDS_TEMPLATE_NUMBERS': (
            '2 0 0 0 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 255 255 255 255 255 255'
        ),
        'GRIB_PDS_PDTN': '0',
        'GRIB_COMMENT': 'Ice cover [Proportion]',
        'GRIB_SHORT_NAME': '0-SFC',
        'GRIB_ELEMENT': 'ICEC',
    }
    self._assertBandMetadataSubset(band, expected_md)

  def test_grib_grib1_read_rotated_pole_lonlat(self):
    ds = gdal.Open('/vsisparse/' + self.getTestFilePath('rotated_pole.grb.xml'))
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterXSize, 726)
    self.assertEqual(ds.RasterYSize, 550)

    band = ds.GetRasterBand(1)
    self.assertIsNone(band.GetNoDataValue())
    self.assertIsNone(band.GetNoDataValue())  # Test caching.

    expected_projection = (
        'GEOGCRS["Coordinate System imported from GRIB file",'
        'BASEGEOGCRS["Coordinate System imported from GRIB file",'
        'DATUM["unnamed",ELLIPSOID["Sphere",6367470,0,'
        'LENGTHUNIT["metre",1,ID["EPSG",9001]]]],'
        'PRIMEM["Greenwich",0,ANGLEUNIT["degree",0.0174532925199433,'
        'ID["EPSG",9122]]]],'
        'DERIVINGCONVERSION["Pole rotation (GRIB convention)",'
        'METHOD["Pole rotation (GRIB convention)"],'
        'PARAMETER["Latitude of the southern pole (GRIB convention)",-30,'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]],'
        'PARAMETER["Longitude of the southern pole (GRIB convention)",-15,'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]],'
        'PARAMETER["Axis rotation (GRIB convention)",0,'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]]],'
        'CS[ellipsoidal,2],'
        'AXIS["latitude",north,ORDER[1],'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]],'
        'AXIS["longitude",east,ORDER[2],'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]]]'
    )
    self.assertEqual(ds.GetProjectionRef(), expected_projection)
    self.assertTrue(ds.GetSpatialRef().IsDerivedGeographic())

    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(), (-30.25, 0.1, 0.0, 24.15, 0.0, -0.1), delta=1e-3
    )

    expected_md = {
        'GRIB_REF_TIME': '1503295200',
        'GRIB_VALID_TIME': '1503295200',
        'GRIB_FORECAST_SECONDS': '0',
        'GRIB_UNIT': '[m^2/s^2]',
        'GRIB_COMMENT': 'Geopotential [m^2/s^2]',
        'GRIB_SHORT_NAME': '0-HTGL',
        'GRIB_ELEMENT': 'GP',
    }
    self._assertBandMetadataSubset(band, expected_md)

  def test_grib_grib2_read_rotated_pole_lonlat(self):
    ds = gdal.Open(
        '/vsisparse/' + self.getTestFilePath('rotated_pole.grb2.xml')
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterXSize, 1102)
    self.assertEqual(ds.RasterYSize, 1076)

    expected_projection = (
        'GEOGCRS["Coordinate System imported from GRIB file",'
        'BASEGEOGCRS["Coordinate System imported from GRIB file",'
        'DATUM["unnamed",ELLIPSOID["Sphere",6371229,0,'
        'LENGTHUNIT["metre",1,ID["EPSG",9001]]]],'
        'PRIMEM["Greenwich",0,ANGLEUNIT["degree",0.0174532925199433,'
        'ID["EPSG",9122]]]],'
        'DERIVINGCONVERSION["Pole rotation (GRIB convention)",'
        'METHOD["Pole rotation (GRIB convention)"],'
        'PARAMETER["Latitude of the southern pole (GRIB convention)",'
        '-31.758312,ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]],'
        'PARAMETER["Longitude of the southern pole (GRIB convention)",'
        '-92.402969,ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]],'
        'PARAMETER["Axis rotation (GRIB convention)",0,'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]]],'
        'CS[ellipsoidal,2],'
        'AXIS["latitude",north,ORDER[1],'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]],'
        'AXIS["longitude",east,ORDER[2],'
        'ANGLEUNIT["degree",0.0174532925199433,ID["EPSG",9122]]]]'
    )
    self.assertEqual(ds.GetProjectionRef(), expected_projection)

    expected_gt = (
        -62.6222310049955,
        0.09000000999091741,
        0.0,
        48.28500200186046,
        0.0,
        -0.09000000372093023,
    )
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(), expected_gt, rel=1e-3
    )

  def test_grib_grib2_read_template_4_40(self):
    if not _has_jp2k_driver():
      self.skipTest('No JPEG2000 driver')
    ds = gdal.Open(self.getTestFilePath('template_4_40.grb2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    expected_md = {
        'GRIB_REF_TIME': '1505088000',
        'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
            '20 0 40008 0 255 99 0 0 1 0 1 -127 -2147483647 255 -127'
            ' -2147483647'
        ),
        'GRIB_VALID_TIME': '1505088000',
        'GRIB_FORECAST_SECONDS': '0',
        'GRIB_UNIT': '[kg/(m^3)]',
        'GRIB_PDS_TEMPLATE_NUMBERS': (
            '20 0 156 72 0 255 99 0 0 0 1 0 0 0 0 1 255 255 255 255 255 255'
            ' 255 255 255 255 255'
        ),
        'GRIB_PDS_PDTN': '40',
        'GRIB_COMMENT': 'Mass Density (Concentration) [kg/(m^3)]',
        'GRIB_SHORT_NAME': '0-SFC',
        'GRIB_ELEMENT': 'MASSDEN',
    }
    self._assertBandMetadataSubset(ds.GetRasterBand(1), expected_md)

  def test_grib_grib2_read_template_4_unhandled(self):
    with gdal.quiet_errors():
      ds = gdal.Open(self.getTestFilePath('template_4_65535.grb2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    expected_md = {
        'GRIB_PDS_TEMPLATE_NUMBERS': '0 1 2 3 4 5',
        'GRIB_PDS_PDTN': '65535',
    }
    self._assertBandMetadataSubset(ds.GetRasterBand(1), expected_md)

  @parameterized.named_parameters(
      (
          'transverse_mercator',
          'transverse_mercator.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["unnamed",SPHEROID["Sphere",6367470,0]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],PROJECTION["Transverse_Mercator"],'
              'PARAMETER["latitude_of_origin",0],'
              'PARAMETER["central_meridian",-117],'
              'PARAMETER["scale_factor",0.9996],'
              'PARAMETER["false_easting",500000],'
              'PARAMETER["false_northing",0],UNIT["Metre",1],'
              'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
          ),
          (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0),
      ),
      (
          'mercator',
          'mercator.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],PROJECTION["Mercator_1SP"],'
              'PARAMETER["central_meridian",0],PARAMETER["scale_factor",1],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["Metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
          ),
          (-13095853.598139772, 72.237, 0.0, 3991876.4600486886, 0.0, -72.237),
      ),
      (
          'mercator_2sp',
          'mercator_2sp.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],PROJECTION["Mercator_2SP"],'
              'PARAMETER["standard_parallel_1",33.5],'
              'PARAMETER["central_meridian",0],PARAMETER["false_easting",0],'
              'PARAMETER["false_northing",0],UNIT["Metre",1],'
              'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
          ),
          (-10931598.94836207, 60.299, 0.0, 3332168.629121481, 0.0, -60.299),
      ),
      (
          'lcc',
          'lambert_conformal_conic.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],PROJECTION["Lambert_Conformal_Conic_2SP"],'
              'PARAMETER["latitude_of_origin",33.5],'
              'PARAMETER["central_meridian",117],'
              'PARAMETER["standard_parallel_1",33],'
              'PARAMETER["standard_parallel_2",34],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["Metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
          ),
          (8974734.737685828, 60.021, 0.0, 6235918.9698001575, 0.0, -60.021),
      ),
      (
          'polar_stereo',
          'polar_stereographic.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],PROJECTION["Polar_Stereographic"],'
              'PARAMETER["latitude_of_origin",60],'
              'PARAMETER["central_meridian",0],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["Metre",1],AXIS["Easting",SOUTH],AXIS["Northing",SOUTH]]'
          ),
          (-5621962.072511509, 71.86, 0.0, 2943991.8007649644, 0.0, -71.86),
      ),
      (
          'aea',
          'albers_equal_area.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],PROJECTION["Albers_Conic_Equal_Area"],'
              'PARAMETER["latitude_of_center",33.5],'
              'PARAMETER["longitude_of_center",117],'
              'PARAMETER["standard_parallel_1",33],'
              'PARAMETER["standard_parallel_2",34],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["Metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
          ),
          (8974979.714292033, 60.022, 0.0, 6235686.52464211, 0.0, -60.022),
      ),
      (
          'laea',
          'lambert_azimuthal_equal_area.grb2',
          (
              'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
              ' file",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563]],'
              'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
              'AUTHORITY["EPSG","9122"]]],'
              'PROJECTION["Lambert_Azimuthal_Equal_Area"],'
              'PARAMETER["latitude_of_center",33.5],'
              'PARAMETER["longitude_of_center",-117],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["Metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
          ),
          (-59384.01063035424, 60.021, 0.0, 44812.5792223211, 0.0, -60.021),
      ),
  )
  def test_grib_grib2_read_projections(
      self,
      filename: str,
      expected_projection: str,
      expected_gt: tuple[float, ...],
  ):
    ds = gdal.Open(self.getTestFilePath(filename))
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetProjectionRef(), expected_projection)
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(), expected_gt, delta=1e-3
    )

  def test_grib_grib2_read_template_5_4_grid_point_ieee_floating_point(self):
    for filename in ('ieee754_single.grb2', 'ieee754_double.grb2'):
      ds = gdal.Open(self.getTestFilePath(filename))
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4727)

  def test_grib_grib2_read_section_5_nbits_zero_decimal_scaled(self):
    ds = gdal.Open(
        self.getTestFilePath('simple_packing_nbits_zero_decimal_scaled.grb2')
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 5)

    if gdal.GetDriverByName('PNG') is not None:
      ds = gdal.Open(self.getTestFilePath('png_nbits_zero_decimal_scaled.grb2'))
      self.assertIsNotNone(ds)
      assert ds is not None
      # google3 builds g2clib without USE_PNG, so Template 5.41 decoding fails
      # (upstream expects checksum 5).
      with gdal.quiet_errors():
        self.assertEqual(ds.GetRasterBand(1).Checksum(), -1)

    if _has_jp2k_driver():
      ds = gdal.Open(
          self.getTestFilePath('jpeg2000_nbits_zero_decimal_scaled.grb2')
      )
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 5)

  def test_grib_grib2_read_spatial_differencing_order_1(self):
    ds = gdal.Open(self.getTestFilePath('spatial_differencing_order_1.grb2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 46650)

  def test_grib_grib2_write_creation_options(self):
    byte_tif = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    tmpfilename = '/vsimem/out_creation_options.grb2'

    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      out_ds = gdal.Translate(
          tmpfilename,
          byte_tif,
          format='GRIB',
          creationOptions=[
              'DISCIPLINE=1',
              (
                  'IDS=CENTER=85(Toulouse) SUBCENTER=2 MASTER_TABLE=5'
                  ' LOCAL_TABLE=0 SIGNF_REF_TIME=0(Analysis)'
                  ' REF_TIME=2017-09-11T12:34:56Z PROD_STATUS=2(Research)'
                  ' TYPE=0(Analysis)'
              ),
              'IDS_SUBCENTER=3',
              'PDS_PDTN=30',
              'BAND_1_PDS_PDTN=40',
              (
                  'PDS_TEMPLATE_NUMBERS=20 0 156 72 0 255 99 0 0 0 1 0 0 0 0 1'
                  ' 255 255 255 255 255 255 255 255 255 255 255'
              ),
          ],
      )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_IDS': (
                  'CENTER=85(Toulouse) SUBCENTER=3 MASTER_TABLE=5 LOCAL_TABLE=0'
                  ' SIGNF_REF_TIME=0(Analysis) REF_TIME=2017-09-11T12:34:56Z'
                  ' PROD_STATUS=2(Research) TYPE=0(Analysis)'
              ),
              'GRIB_PDS_TEMPLATE_NUMBERS': (
                  '20 0 156 72 0 255 99 0 0 0 1 0 0 0 0 1 255 255 255 255 255'
                  ' 255 255 255 255 255 255'
              ),
              'GRIB_DISCIPLINE': '1(Hydrological)',
              'GRIB_PDS_PDTN': '40',
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_NUMBERS with more elements than needed (warning).
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=40',
                (
                    'PDS_TEMPLATE_NUMBERS=20 0 156 72 0 255 99 0 0 0 1 0 0 0 0'
                    ' 1 255 255 255 255 255 255 255 255 255 255 255 0extra'
                ),
            ],
        )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '40',
              'GRIB_PDS_TEMPLATE_NUMBERS': (
                  '20 0 156 72 0 255 99 0 0 0 1 0 0 0 0 1 255 255 255 255 255'
                  ' 255 255 255 255 255 255 0'
              ),
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_NUMBERS with insufficient number of elements.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=40',
                (
                    'PDS_TEMPLATE_NUMBERS=20 0 156 72 0 255 99 0 0 0 1 0 0 0 0'
                    ' 1 255 255 255 255 255 255 255 255 255 255'
                ),
            ],
        )
      self.assertIsNone(out_ds)
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_ASSEMBLED_VALUES.
      out_ds = gdal.Translate(
          tmpfilename,
          byte_tif,
          format='GRIB',
          creationOptions=[
              'PDS_PDTN=40',
              (
                  'PDS_TEMPLATE_ASSEMBLED_VALUES=20 0 40008 0 255 99 0 0 1 0 1'
                  ' -127 -2147483647 255 -127 -2147483647'
              ),
          ],
      )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '40',
              'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
                  '20 0 40008 0 255 99 0 0 1 0 1 -127 -2147483647 255 -127'
                  ' -2147483647'
              ),
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_ASSEMBLED_VALUES with extra elements (warning).
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=40',
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=20 0 40008 0 255 99 0 0 1 0'
                    ' 1 -127 -2147483647 255 -127 -2147483647 0extra'
                ),
            ],
        )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '40',
              'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
                  '20 0 40008 0 255 99 0 0 1 0 1 -127 -2147483647 255 -127'
                  ' -2147483647'
              ),
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_ASSEMBLED_VALUES with insufficient elements.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=40',
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=20 0 40008 0 255 99 0 0 1 0'
                    ' 1 -127 -2147483647 255 -127'
                ),
            ],
        )
      self.assertIsNone(out_ds)
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_ASSEMBLED_VALUES with variable number of elements.
      out_ds = gdal.Translate(
          tmpfilename,
          byte_tif,
          format='GRIB',
          creationOptions=[
              'PDS_PDTN=32',
              (
                  'PDS_TEMPLATE_ASSEMBLED_VALUES=5 7 2 0 0 0 0 1 0 2 31 285'
                  ' 17292 2 61145 31 285 17292 2 61145'
              ),
          ],
      )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '32',
              'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
                  '5 7 2 0 0 0 0 1 0 2 31 285 17292 2 61145 31 285 17292 2'
                  ' 61145'
              ),
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # Variable elements with insufficient number in variable section.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=32',
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=5 7 2 0 0 0 0 1 0 2 31 285'
                    ' 17292 2 61145 31 285 17292 2'
                ),
            ],
        )
      self.assertIsNone(out_ds)
      gdal.Unlink(tmpfilename)

      # Variable elements with extra elements.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=32',
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=5 7 2 0 0 0 0 1 0 2 31 285'
                    ' 17292 2 61145 31 285 17292 2 61145 0extra'
                ),
            ],
        )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '32',
              'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
                  '5 7 2 0 0 0 0 1 0 2 31 285 17292 2 61145 31 285 17292 2'
                  ' 61145'
              ),
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # PDS_TEMPLATE_NUMBERS with variable number of elements.
      out_ds = gdal.Translate(
          tmpfilename,
          byte_tif,
          format='GRIB',
          creationOptions=[
              'PDS_PDTN=32',
              (
                  'PDS_TEMPLATE_NUMBERS=5 7 2 0 0 0 0 0 1 0 0 0 0 2 0 31 1 29'
                  ' 67 140 2 0 0 238 217 0 31 1 29 67 140 2 0 0 238 217'
              ),
          ],
      )
      self.assertIsNotNone(out_ds)
      out_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '32',
              'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES': (
                  '5 7 2 0 0 0 0 1 0 2 31 285 17292 2 61145 31 285 17292 2'
                  ' 61145'
              ),
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # Unknown PDS_PDTN with PDS_TEMPLATE_NUMBERS.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=65535',
                'PDS_TEMPLATE_NUMBERS=1 2 3 4 5',
            ],
        )
      self.assertIsNotNone(out_ds)
      out_ds = None
      with gdal.quiet_errors():
        ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertBandMetadataSubset(
          ds.GetRasterBand(1),
          {
              'GRIB_PDS_PDTN': '65535',
              'GRIB_PDS_TEMPLATE_NUMBERS': '1 2 3 4 5',
          },
      )
      ds = None
      gdal.Unlink(tmpfilename)

      # Unknown PDS_PDTN with PDS_TEMPLATE_ASSEMBLED_VALUES.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=65535',
                'PDS_TEMPLATE_ASSEMBLED_VALUES=1 2 3 4 5',
            ],
        )
      self.assertIsNone(out_ds)
      gdal.Unlink(tmpfilename)

      # PDS_PDTN != 0 without template numbers.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=['PDS_PDTN=32'],
        )
      self.assertIsNone(out_ds)
      gdal.Unlink(tmpfilename)

      # Invalid values in PDS_TEMPLATE_NUMBERS.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=254',
                'PDS_TEMPLATE_NUMBERS=-1 256 0 0 0 0',
            ],
        )
      self.assertIsNotNone(out_ds)
      out_ds = None
      gdal.Unlink(tmpfilename)

      # Invalid values in PDS_TEMPLATE_ASSEMBLED_VALUES.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=44',
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=-1 256 -1 1 128 4000000000'
                    ' -1 -4 1 1 1 65536 1 1 32768 1 -129 -4 1 -1 -4'
                ),
            ],
        )
      self.assertIsNotNone(out_ds)
      out_ds = None
      gdal.Unlink(tmpfilename)

      # Both PDS_TEMPLATE_NUMBERS and PDS_TEMPLATE_ASSEMBLED_VALUES.
      with gdal.quiet_errors():
        out_ds = gdal.Translate(
            tmpfilename,
            byte_tif,
            format='GRIB',
            creationOptions=[
                'PDS_PDTN=40',
                (
                    'PDS_TEMPLATE_NUMBERS=20 0 156 72 0 255 99 0 0 0 1 0 0 0 0'
                    ' 1 255 255 255 255 255 255 255 255 255 255 255'
                ),
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=20 0 40008 0 255 99 0 0 1 0'
                    ' 1 -127 -2147483647 255 -127 -2147483647'
                ),
            ],
        )
      self.assertIsNone(out_ds)

  def test_grib_grib2_write_projections(self):
    tmpfilename = '/vsimem/out_write_projections.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      filenames = [
          'albers_equal_area.grb2',
          'lambert_azimuthal_equal_area.grb2',
          'lambert_conformal_conic.grb2',
          'mercator.grb2',
          'mercator_2sp.grb2',
          'polar_stereographic.grb2',
          'ieee754_single.grb2',
      ]
      for filename in filenames:
        filepath = self.getTestFilePath(filename)
        src_ds = gdal.Open(filepath)
        self.assertIsNotNone(src_ds)
        assert src_ds is not None
        gdal.Translate(tmpfilename, filepath, format='GRIB')
        out_ds = gdal.Open(tmpfilename)
        self.assertIsNotNone(out_ds)
        assert out_ds is not None
        self.assertEqual(
            src_ds.GetProjectionRef(),
            out_ds.GetProjectionRef(),
            msg=f'Projection mismatch for {filename}',
        )
        self._assertGeoTransformAlmostEqual(
            out_ds.GetGeoTransform(), src_ds.GetGeoTransform(), delta=1e-5
        )
        out_ds = None
        gdal.Unlink(tmpfilename)

      # Test writing GRS80.
      src_ds = gdal.GetDriverByName('MEM').Create('', 2, 2, 1, gdal.GDT_Float32)
      self.assertIsNotNone(src_ds)
      assert src_ds is not None
      src_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
      src_ds.SetProjection(
          'GEOGCS["GRS 1980(IUGG, 1980)",'
          'DATUM["unknown",SPHEROID["GRS80",6378137,298.257222101]],'
          'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]]'
      )
      out_ds = self.driver.CreateCopy(tmpfilename, src_ds)
      self.assertIsNotNone(out_ds)
      assert out_ds is not None
      wkt = out_ds.GetProjectionRef()
      out_ds = None
      gdal.Unlink(tmpfilename)
      self.assertIn('SPHEROID["GRS80",6378137,298.257222101]', wkt)

      byte_tif = gdrivers_util.GetTestFilePath('gtiff/byte.tif')

      # Test writing Mercator_1SP with scale != 1 (read as Mercator_2SP).
      src_ds = gdal.Warp(
          '',
          byte_tif,
          format='MEM',
          dstSRS=(
              'PROJCS["unnamed",GEOGCS["NAD27",'
              'DATUM["North_American_Datum_1927",'
              'SPHEROID["Clarke 1866",6378206.4,294.9786982138982,'
              'AUTHORITY["EPSG","7008"]],AUTHORITY["EPSG","6267"]],'
              'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
              'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
              'AUTHORITY["EPSG","4267"]],PROJECTION["Mercator_1SP"],'
              'PARAMETER["central_meridian",0],'
              'PARAMETER["scale_factor",0.8347374126019634],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["metre",1,AUTHORITY["EPSG","9001"]]]'
          ),
      )
      gdal.Translate(tmpfilename, src_ds, format='GRIB')
      out_ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(out_ds)
      assert out_ds is not None
      expected_wkt = (
          'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
          ' file",DATUM["unnamed",SPHEROID["Spheroid imported from GRIB'
          ' file",6378206.4,294.978698213911]],PRIMEM["Greenwich",0],'
          'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],'
          'PROJECTION["Mercator_2SP"],PARAMETER["standard_parallel_1",33.500986],'
          'PARAMETER["central_meridian",0],PARAMETER["false_easting",0],'
          'PARAMETER["false_northing",0],UNIT["Metre",1],'
          'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
      )
      got_sr = osr.SpatialReference()
      got_sr.SetFromUserInput(out_ds.GetProjectionRef())
      expected_sr = osr.SpatialReference()
      expected_sr.SetFromUserInput(expected_wkt)
      self.assertIsSame(got_sr, expected_sr)
      self._assertGeoTransformAlmostEqual(
          out_ds.GetGeoTransform(),
          (-10931635.565066436, 60.297, 0.0, 3331982.221608528, 0.0, -60.297),
          delta=1e-5,
      )
      out_ds = None
      gdal.Unlink(tmpfilename)

      # Test writing LCC_1SP (read as LCC_2SP).
      src_ds = gdal.Warp(
          '',
          byte_tif,
          format='MEM',
          dstSRS=(
              'PROJCS["unnamed",GEOGCS["NAD27",'
              'DATUM["North_American_Datum_1927",'
              'SPHEROID["Clarke 1866",6378206.4,294.9786982138982,'
              'AUTHORITY["EPSG","7008"]],AUTHORITY["EPSG","6267"]],'
              'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
              'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
              'AUTHORITY["EPSG","4267"]],'
              'PROJECTION["Lambert_Conformal_Conic_1SP"],'
              'PARAMETER["latitude_of_origin",33.5],'
              'PARAMETER["central_meridian",117],'
              'PARAMETER["scale_factor",0.9999],'
              'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
              'UNIT["metre",1]]'
          ),
      )
      gdal.Translate(tmpfilename, src_ds, format='GRIB')
      out_ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(out_ds)
      assert out_ds is not None
      expected_wkt = (
          'PROJCS["unnamed",GEOGCS["Coordinate System imported from GRIB'
          ' file",DATUM["unnamed",SPHEROID["Spheroid imported from GRIB'
          ' file",6378206.4,294.978698213911]],PRIMEM["Greenwich",0],'
          'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],'
          'PROJECTION["Lambert_Conformal_Conic_2SP"],'
          'PARAMETER["latitude_of_origin",33.5],'
          'PARAMETER["central_meridian",117],'
          'PARAMETER["standard_parallel_1",34.310911],'
          'PARAMETER["standard_parallel_2",32.686501],'
          'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
          'UNIT["Metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
      )
      got_sr = osr.SpatialReference()
      got_sr.SetFromUserInput(out_ds.GetProjectionRef())
      expected_sr = osr.SpatialReference()
      expected_sr.SetFromUserInput(expected_wkt)
      self.assertIsSame(got_sr, expected_sr)
      self._assertGeoTransformAlmostEqual(
          out_ds.GetGeoTransform(),
          (8974472.884926716, 60.017, 0.0, 6235685.688523474, 0.0, -60.017),
          delta=1e-5,
      )
      out_ds = None

  def test_grib_grib2_write_rotated_lat_lon_from_projstring(self):
    self._assertWriteRotatedLatLon(
        'write_rotated_lat_lon_from_projstring',
        '+proj=ob_tran +o_proj=longlat +o_lon_p=-10 +o_lat_p=39.25 +lon_0=18'
        ' +a=6367470 +rf=594.313048347956 +no_defs',
    )

  def test_grib_grib2_write_rotated_lat_lon_from_grib_convention(self):
    self._assertWriteRotatedLatLon(
        'write_rotated_lat_lon_from_grib_convention',
        """GEOGCRS["Coordinate System imported from GRIB file",
    BASEGEOGCRS["Coordinate System imported from GRIB file",
        DATUM["unnamed",
            ELLIPSOID["Spheroid imported from GRIB file",6367470,594.313048347956,
                LENGTHUNIT["metre",1,
                    ID["EPSG",9001]]]],
        PRIMEM["Greenwich",0,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]]],
    DERIVINGCONVERSION["Pole rotation (GRIB convention)",
        METHOD["Pole rotation (GRIB convention)"],
        PARAMETER["Latitude of the southern pole (GRIB convention)",-39.25,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]],
        PARAMETER["Longitude of the southern pole (GRIB convention)",18,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]],
        PARAMETER["Axis rotation (GRIB convention)",10,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]]],
    CS[ellipsoidal,2],
        AXIS["latitude",north,
            ORDER[1],
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]],
        AXIS["longitude",east,
            ORDER[2],
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]]]""",
    )

  def test_grib_grib2_write_rotated_lat_lon_from_netcdf_convention(self):
    self._assertWriteRotatedLatLon(
        'write_rotated_lat_lon_from_netcdf_convention',
        """GEOGCRS["Rotated_pole",
    BASEGEOGCRS["unknown",
        DATUM["unnamed",
            ELLIPSOID["Spheroid",6367470,594.313048347956,
                LENGTHUNIT["metre",1,
                    ID["EPSG",9001]]]],
        PRIMEM["Greenwich",0,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]]],
    DERIVINGCONVERSION["Pole rotation (netCDF CF convention)",
        METHOD["Pole rotation (netCDF CF convention)"],
        PARAMETER["Grid north pole latitude (netCDF CF convention)",39.25,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]],
        PARAMETER["Grid north pole longitude (netCDF CF convention)",-162,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]],
        PARAMETER["North pole grid longitude (netCDF CF convention)",-10,
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]]],
    CS[ellipsoidal,2],
        AXIS["latitude",north,
            ORDER[1],
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]],
        AXIS["longitude",east,
            ORDER[2],
            ANGLEUNIT["degree",0.0174532925199433,
                ID["EPSG",9122]]]]""",
    )

  def test_grib_grib2_write_data_encodings(self):
    gs5_simple = 0
    gs5_cmplx = 2
    gs5_cmplxsec = 3
    gs5_ieee = 4
    gs5_jpeg2000 = 40
    gs5_png = 41

    byte_tif = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    ds_mint = self.getTestFilePath('ds.mint.bin')
    int16_tif = gcore_util.GetTestFilePath('int16.tif')
    uint16_tif = gcore_util.GetTestFilePath('uint16.tif')
    int32_tif = gcore_util.GetTestFilePath('int32.tif')
    uint32_tif = gcore_util.GetTestFilePath('uint32.tif')
    float32_tif = gcore_util.GetTestFilePath('float32.tif')
    float64_tif = gcore_util.GetTestFilePath('float64.tif')
    utm_tif = gdrivers_util.GetTestFilePath('gtiff/utm.tif')

    tests: list[tuple[Any, list[str], int | tuple[int, ...], int]] = [
        (byte_tif, [], 4672, gs5_simple),
        (byte_tif, ['DATA_ENCODING=SIMPLE_PACKING'], 4672, gs5_simple),
        (byte_tif, ['DATA_ENCODING=IEEE_FLOATING_POINT'], 4672, gs5_ieee),
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'NBITS=8'],
            4672,
            gs5_simple,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'NBITS=9'],
            4672,
            gs5_simple,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'NBITS=7'],
            4484,
            gs5_simple,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'DECIMAL_SCALE_FACTOR=-1'],
            4820,
            gs5_simple,
        ),
        (
            byte_tif,
            [
                'DATA_ENCODING=SIMPLE_PACKING',
                'NBITS=5',
                'DECIMAL_SCALE_FACTOR=-1',
            ],
            4820,
            gs5_simple,
        ),
        (
            byte_tif,
            [
                'DATA_ENCODING=SIMPLE_PACKING',
                'NBITS=8',
                'DECIMAL_SCALE_FACTOR=-1',
            ],
            4855,
            gs5_simple,
        ),
        (
            ds_mint,
            [
                'PDS_PDTN=8',
                (
                    'PDS_TEMPLATE_ASSEMBLED_VALUES=0 5 2 0 0 255 255 1 19 1 0 0'
                    ' 255 -1 -2147483647 2008 2 22 12 0 0 1 0 3 255 1 12 1 0'
                ),
            ],
            46650,
            gs5_cmplx,
        ),
        (byte_tif, ['DATA_ENCODING=COMPLEX_PACKING'], 4672, gs5_cmplx),
        (
            byte_tif,
            ['DATA_ENCODING=COMPLEX_PACKING', 'SPATIAL_DIFFERENCING_ORDER=0'],
            4672,
            gs5_cmplx,
        ),
        (byte_tif, ['SPATIAL_DIFFERENCING_ORDER=1'], 4672, gs5_cmplxsec),
        (
            byte_tif,
            ['DATA_ENCODING=COMPLEX_PACKING', 'SPATIAL_DIFFERENCING_ORDER=1'],
            4672,
            gs5_cmplxsec,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=COMPLEX_PACKING', 'SPATIAL_DIFFERENCING_ORDER=2'],
            4672,
            gs5_cmplxsec,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=COMPLEX_PACKING', 'DECIMAL_SCALE_FACTOR=-1'],
            4820,
            gs5_cmplx,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=COMPLEX_PACKING', 'NBITS=7'],
            4484,
            gs5_cmplx,
        ),
    ]

    if gdal.GetDriverByName('PNG') is not None:
      tests += [
          (byte_tif, ['DATA_ENCODING=PNG'], 4672, gs5_png),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=8'], 4672, gs5_png),
          (
              byte_tif,
              ['DATA_ENCODING=PNG', 'DECIMAL_SCALE_FACTOR=-1'],
              4820,
              gs5_png,
          ),
          (
              byte_tif,
              ['DATA_ENCODING=PNG', 'NBITS=8', 'DECIMAL_SCALE_FACTOR=-1'],
              4855,
              gs5_png,
          ),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=9'], 4672, gs5_png),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=7'], 4672, gs5_png),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=4'], 5103, gs5_png),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=3'], 5103, gs5_png),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=2'], 5103, gs5_png),
          (byte_tif, ['DATA_ENCODING=PNG', 'NBITS=1'], 5103, gs5_png),
          (float32_tif, ['DATA_ENCODING=PNG'], 4672, gs5_png),
      ]

    found_j2k_drivers = []
    for drvname in ('JP2KAK', 'JP2OPENJPEG', 'JPEG2000'):
      if gdal.GetDriverByName(drvname) is not None:
        found_j2k_drivers.append(drvname)

    if found_j2k_drivers:
      tests += [
          (byte_tif, ['DATA_ENCODING=JPEG2000'], 4672, gs5_jpeg2000),
          (
              byte_tif,
              ['DATA_ENCODING=JPEG2000', 'COMPRESSION_RATIO=2'],
              4672,
              gs5_jpeg2000,
          ),
          (
              byte_tif,
              ['DATA_ENCODING=JPEG2000', 'NBITS=8'],
              4672,
              gs5_jpeg2000,
          ),
          (
              byte_tif,
              ['DATA_ENCODING=JPEG2000', 'DECIMAL_SCALE_FACTOR=-1'],
              (4820, 4722),
              gs5_jpeg2000,
          ),
          (
              byte_tif,
              ['DATA_ENCODING=JPEG2000', 'NBITS=8', 'DECIMAL_SCALE_FACTOR=-1'],
              (4855, 4795),
              gs5_jpeg2000,
          ),
          (
              byte_tif,
              ['DATA_ENCODING=JPEG2000', 'NBITS=9'],
              4672,
              gs5_jpeg2000,
          ),
          (
              byte_tif,
              ['DATA_ENCODING=JPEG2000', 'NBITS=7'],
              (4484, 4899, 4440),
              gs5_jpeg2000,
          ),
          (float32_tif, ['DATA_ENCODING=JPEG2000'], 4672, gs5_jpeg2000),
      ]
      for drvname in found_j2k_drivers:
        tests.append(
            (byte_tif, ['JPEG2000_DRIVER=' + drvname], 4672, gs5_jpeg2000)
        )

    tests += [
        (int16_tif, [], 4672, gs5_simple),
        (uint16_tif, [], 4672, gs5_simple),
        (int32_tif, [], 4672, gs5_simple),
        (uint32_tif, [], 4672, gs5_simple),
        (float32_tif, [], 4672, gs5_ieee),
        (float64_tif, [], 4672, gs5_ieee),
        (float32_tif, ['DATA_ENCODING=IEEE_FLOATING_POINT'], 4672, gs5_ieee),
        (float64_tif, ['DATA_ENCODING=IEEE_FLOATING_POINT'], 4672, gs5_ieee),
        (float32_tif, ['DATA_ENCODING=COMPLEX_PACKING'], 4672, gs5_cmplx),
    ]

    one_ds = self._createMemWgs84Dataset(1, 1)
    one_ds.GetRasterBand(1).Fill(1)
    tests += [
        (one_ds, [], 1, gs5_simple),
        (one_ds, ['DATA_ENCODING=COMPLEX_PACKING'], 1, gs5_cmplx),
    ]
    if gdal.GetDriverByName('PNG') is not None:
      tests.append((one_ds, ['DATA_ENCODING=PNG'], 1, gs5_png))
    if found_j2k_drivers:
      tests.append((one_ds, ['DATA_ENCODING=JPEG2000'], 1, gs5_jpeg2000))

    nodata_never_hit_ds = self._createMemWgs84Dataset(1, 1)
    nodata_never_hit_ds.GetRasterBand(1).SetNoDataValue(1)
    tests.append((nodata_never_hit_ds, [], 0, gs5_simple))

    all_nodata_ds = self._createMemWgs84Dataset(1, 1)
    all_nodata_ds.GetRasterBand(1).SetNoDataValue(0)
    tests.append(
        (all_nodata_ds, ['DATA_ENCODING=COMPLEX_PACKING'], 0, gs5_cmplx)
    )

    tmpfilename = '/vsimem/out_data_encodings.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      for (
          filename,
          options,
          expected_cs,
          expected_section5_template_number,
      ) in tests:
        gdal.ErrorReset()
        gdal.Translate(
            tmpfilename, filename, format='GRIB', creationOptions=options
        )
        self.assertEqual(
            gdal.GetLastErrorMsg(),
            '',
            msg=f'Did not expect error for {filename}, {options}',
        )

        section5 = _grib_read_section(tmpfilename, 5)
        self.assertIsNotNone(section5)
        assert section5 is not None
        section5_template_number = struct.unpack('>h', section5[9:11])[0]
        self.assertEqual(
            section5_template_number,
            expected_section5_template_number,
            msg=f'Wrong section 5 template for {filename}, {options}',
        )

        out_ds = gdal.Open(tmpfilename)
        self.assertIsNotNone(out_ds)
        assert out_ds is not None
        if section5_template_number == gs5_png:
          # google3 builds g2clib without USE_PNG; Template 5.41 encoding via
          # the PNG driver succeeds, while reading pixel data back fails.
          with gdal.quiet_errors():
            cs = out_ds.GetRasterBand(1).Checksum()
          self.assertEqual(cs, -1)
        else:
          cs = out_ds.GetRasterBand(1).Checksum()
          expected_cs_tuple = (
              expected_cs if isinstance(expected_cs, tuple) else (expected_cs,)
          )
          self.assertIn(
              cs,
              expected_cs_tuple,
              msg=f'Unexpected checksum for {filename}, {options}',
          )
        nd = out_ds.GetRasterBand(1).GetNoDataValue()
        out_ds = None
        gdal.Unlink(tmpfilename)

        if section5_template_number in (gs5_cmplx, gs5_cmplxsec):
          ref_ds = (
              gdal.Open(filename) if isinstance(filename, str) else filename
          )
          self.assertIsNotNone(ref_ds)
          assert ref_ds is not None
          self.assertEqual(
              nd,
              ref_ds.GetRasterBand(1).GetNoDataValue(),
              msg=f'Unexpected nodata for {filename}, {options}',
          )

      # Test floating point data with dynamic < 1.
      test_ds = self._createMemWgs84Dataset(2, 2, gdal.GDT_Float32)
      test_ds.WriteRaster(
          0, 0, 2, 2, struct.pack(4 * 'f', 1.23, 1.45, 1.56, 1.78)
      )

      # google3 builds g2clib without USE_PNG, so skip PNG read-back checks.
      encodings = ['SIMPLE_PACKING', 'COMPLEX_PACKING', 'IEEE_FLOATING_POINT']
      if found_j2k_drivers and found_j2k_drivers != ['JPEG2000']:
        encodings.append('JPEG2000')

      for encoding in encodings:
        gdal.ErrorReset()
        options = ['DATA_ENCODING=' + encoding]
        if encoding == 'COMPLEX_PACKING':
          with gdal.quiet_errors():
            success = gdal.Translate(
                tmpfilename, test_ds, format='GRIB', creationOptions=options
            )
          self.assertIsNone(
              success, msg=f'Expected error for dynamic < 1, {options}'
          )
        else:
          gdal.Translate(
              tmpfilename, test_ds, format='GRIB', creationOptions=options
          )
          self.assertEqual(
              gdal.GetLastErrorMsg(),
              '',
              msg=f'Did not expect error for dynamic < 1, {options}',
          )
          out_ds = gdal.Open(tmpfilename)
          self.assertIsNotNone(out_ds)
          assert out_ds is not None
          got_vals = struct.unpack(4 * 'd', out_ds.ReadRaster())
          out_ds = None
          if encoding == 'IEEE_FLOATING_POINT':
            expected_vals = (1.23, 1.45, 1.56, 1.78)
          else:
            expected_vals = (
                1.2300000190734863,
                1.4487500190734863,
                1.5581250190734863,
                1.7807812690734863,
            )
          self.assertIterAlmostEqual(got_vals, expected_vals, delta=1e-7)
        gdal.Unlink(tmpfilename)

      # Test floating point data with very large dynamic.
      test_ds = self._createMemWgs84Dataset(2, 2, gdal.GDT_Float32)
      test_ds.WriteRaster(
          0,
          0,
          2,
          2,
          struct.pack(4 * 'f', 1.23e10, -2.45e10, 1.23e10, -2.45e10),
      )

      # google3 builds g2clib without USE_PNG, so skip PNG read-back checks.
      encodings = ['SIMPLE_PACKING']
      if found_j2k_drivers:
        encodings.append('JPEG2000')

      for encoding in encodings:
        if encoding != 'SIMPLE_PACKING':
          gdal.PushErrorHandler()
        try:
          gdal.Translate(
              tmpfilename,
              test_ds,
              format='GRIB',
              creationOptions=['DATA_ENCODING=' + encoding],
          )
        finally:
          if encoding != 'SIMPLE_PACKING':
            gdal.PopErrorHandler()
        out_ds = gdal.Open(tmpfilename)
        self.assertIsNotNone(
            out_ds, msg=f'Failed to re-open dataset for {encoding}'
        )
        assert out_ds is not None
        got_vals = struct.unpack(4 * 'd', out_ds.ReadRaster())
        out_ds = None
        gdal.Unlink(tmpfilename)
        expected_vals = (1.23e10, -2.45e10, 1.23e10, -2.45e10)
        for got, expected in zip(got_vals, expected_vals):
          self.assertLessEqual(
              abs((got - expected) / expected),
              1e-4,
              msg=f'Unexpected values for {encoding}',
          )

      # Test lossy J2K compression.
      for drvname in found_j2k_drivers:
        gdal.ErrorReset()
        gdal.Translate(
            tmpfilename,
            utm_tif,
            format='GRIB',
            creationOptions=[
                'JPEG2000_DRIVER=' + drvname,
                'COMPRESSION_RATIO=20',
            ],
        )
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        out_ds = gdal.Open(tmpfilename)
        self.assertIsNotNone(out_ds)
        assert out_ds is not None
        cs = out_ds.GetRasterBand(1).Checksum()
        out_ds = None
        gdal.Unlink(tmpfilename)
        self.assertNotIn(
            cs,
            (0, 50235),
            msg=f'Unexpected checksum for lossy JPEG2000 with {drvname}',
        )

  def test_grib_grib2_write_data_encodings_warnings_and_errors(self):
    byte_tif = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    ds_mint = self.getTestFilePath('ds.mint.bin')
    cfloat32_tif = gcore_util.GetTestFilePath('cfloat32.tif')
    float64_asc = gdrivers_util.GetTestFilePath('aaigrid/float64.asc')
    rotation_img = gdrivers_util.GetTestFilePath('envi/rotation.img')
    tmpfilename = '/vsimem/out_warnings_errors.grb2'

    warning_cases: list[tuple[str, list[str], int]] = [
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'DECIMAL_SCALE_FACTOR=1'],
            4672,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'JPEG2000_DRIVER=FOO'],
            4672,
        ),
        (
            byte_tif,
            ['DATA_ENCODING=SIMPLE_PACKING', 'SPATIAL_DIFFERENCING_ORDER=1'],
            4672,
        ),
        (ds_mint, ['DATA_ENCODING=SIMPLE_PACKING'], 41640),
        (byte_tif, ['NBITS=32'], 4672),
        (byte_tif, ['DATA_ENCODING=IEEE_FLOATING_POINT', 'NBITS=8'], 4672),
        (
            byte_tif,
            ['DATA_ENCODING=IEEE_FLOATING_POINT', 'DECIMAL_SCALE_FACTOR=-1'],
            4672,
        ),
    ]

    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      for filename, options, expected_cs in warning_cases:
        src_ds = gdal.Open(filename)
        self.assertIsNotNone(src_ds)
        gdal.ErrorReset()
        with gdal.quiet_errors():
          out_ds = self.driver.CreateCopy(tmpfilename, src_ds, options=options)
        self.assertNotEqual(
            gdal.GetLastErrorMsg(),
            '',
            msg=f'Expected warning for {filename}, {options}',
        )
        self.assertIsNotNone(
            out_ds, msg=f'Did not expect null return for {filename}, {options}'
        )
        assert out_ds is not None
        self.assertEqual(out_ds.GetRasterBand(1).Checksum(), expected_cs)
        out_ds = None
        gdal.Unlink(tmpfilename)

      nosrs_vrt = '/vsimem/test_nosrs.vrt'
      huge_tif = '/vsimem/huge.tif'
      with (
          gcore_util.GdalUnlinkWhenDone(nosrs_vrt),
          gcore_util.GdalUnlinkWhenDone(huge_tif),
      ):
        gdal.FileFromMemBuffer(
            nosrs_vrt,
            f"""<VRTDataset rasterXSize="20" rasterYSize="20">
  <VRTRasterBand dataType="Byte" band="1">
    <ColorInterp>Gray</ColorInterp>
    <SimpleSource>
      <SourceFilename relativeToVRT="0">{byte_tif}</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </SimpleSource>
  </VRTRasterBand>
  <GCPList>
    <GCP Id="1" Pixel="0" Line="0" X="0" Y="0"/>
    <GCP Id="2" Pixel="0" Line="50" X="0" Y="50"/>
    <GCP Id="3" Pixel="50" Line="50" X="50" Y="50"/>
    <GCP Id="4" Pixel="50" Line="0" X="50" Y="0"/>
  </GCPList>
</VRTDataset>""",
        )
        gdal.GetDriverByName('GTiff').Create(
            huge_tif, 65535, 65535, 1, options=['SPARSE_OK=YES']
        )

        error_cases: list[tuple[str, list[str]]] = [
            (byte_tif, ['DATA_ENCODING=FOO']),
            (
                byte_tif,
                ['JPEG2000_DRIVER=FOO', 'SPATIAL_DIFFERENCING_ORDER=BAR'],
            ),
            (byte_tif, ['SPATIAL_DIFFERENCING_ORDER=3']),
            (byte_tif, ['JPEG2000_DRIVER=THIS_IS_NOT_A_J2K_DRIVER']),
            (byte_tif, ['JPEG2000_DRIVER=DERIVED']),
            (cfloat32_tif, []),
            (float64_asc, []),
            (nosrs_vrt, []),
            (rotation_img, []),
            (huge_tif, []),
        ]

        for filename, options in error_cases:
          src_ds = gdal.Open(filename)
          self.assertIsNotNone(src_ds, msg=f'Failed to open {filename}')
          gdal.ErrorReset()
          with gdal.quiet_errors():
            out_ds = self.driver.CreateCopy(
                tmpfilename, src_ds, options=options
            )
          self.assertNotEqual(
              gdal.GetLastErrorMsg(),
              '',
              msg=f'Expected error for {filename}, {options}',
          )
          self.assertIsNone(
              out_ds, msg=f'Expected null return for {filename}, {options}'
          )
          gdal.Unlink(tmpfilename)

      with gdal.quiet_errors():
        out_ds = gdal.Translate('/i/do_not/exist.grb2', byte_tif, format='GRIB')
      self.assertIsNone(out_ds)

  def test_grib_grib2_write_temperatures(self):
    tmpfilename = '/vsimem/out_temperatures.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      for src_type, data_encoding, input_unit in (
          (gdal.GDT_Float32, 'IEEE_FLOATING_POINT', None),
          (gdal.GDT_Float32, 'IEEE_FLOATING_POINT', 'C'),
          (gdal.GDT_Float32, 'IEEE_FLOATING_POINT', 'K'),
          (gdal.GDT_Float64, 'IEEE_FLOATING_POINT', None),
          (gdal.GDT_Float32, 'SIMPLE_PACKING', None),
      ):
        src_ds = self._createMemWgs84Dataset(2, 2, src_type)
        src_ds.WriteRaster(
            0,
            0,
            2,
            2,
            struct.pack(4 * 'f', 25.0, 25.1, 25.1, 25.2),
            buf_type=gdal.GDT_Float32,
        )

        options = [
            'DATA_ENCODING=' + data_encoding,
            'PDS_PDTN=8',
            (
                'PDS_TEMPLATE_NUMBERS=0 5 2 0 0 0 255 255 1 0 0 0 43 1 0 0 0 0'
                ' 0 255 129 255 255 255 255 7 216 2 23 12 0 0 1 0 0 0 0 3 255 1'
                ' 0 0 0 12 1 0 0 0 0'
            ),
        ]
        if input_unit is not None:
          options.append('INPUT_UNIT=' + input_unit)
        self.driver.CreateCopy(tmpfilename, src_ds, options=options)

        out_ds = gdal.Open(tmpfilename)
        self.assertIsNotNone(out_ds)
        assert out_ds is not None
        got_vals = struct.unpack(4 * 'd', out_ds.ReadRaster())
        out_ds = None
        gdal.Unlink(tmpfilename)

        if input_unit != 'K':
          expected_vals = (25.0, 25.1, 25.1, 25.2)
        else:
          expected_vals = (
              25.0 - 273.15,
              25.1 - 273.15,
              25.1 - 273.15,
              25.2 - 273.15,
          )
        for got, expected in zip(got_vals, expected_vals):
          self.assertLessEqual(
              abs((got - expected) / expected),
              1e-4,
              msg=f'Failed for {data_encoding}, type={src_type}',
          )

  @parameterized.parameters([gdal.GDT_Byte, gdal.GDT_Float32])
  def test_grib_grib2_write_nodata(self, datatype: int):
    src_ds = self._createMemWgs84Dataset(2, 2, datatype)
    src_ds.GetRasterBand(1).SetNoDataValue(123)
    tmpfilename = '/vsimem/out_write_nodata.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      self.driver.CreateCopy(
          tmpfilename, src_ds, options=['DATA_ENCODING=COMPLEX_PACKING']
      )
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 123)
      got_vals = struct.unpack(4 * 'd', ds.ReadRaster())
      ds = None
      self.assertEqual(got_vals, (0.0, 0.0, 0.0, 0.0))

  @parameterized.parameters([gdal.GDT_Byte, gdal.GDT_Float32])
  def test_grib_grib2_write_nodata_only(self, datatype: int):
    src_ds = self._createMemWgs84Dataset(2, 2, datatype)
    src_ds.GetRasterBand(1).SetNoDataValue(12.3)
    src_ds.WriteRaster(
        0,
        0,
        2,
        2,
        struct.pack(4 * 'f', 12.3, 12.3, 12.3, 12.3),
        buf_type=gdal.GDT_Float32,
    )
    tmpfilename = '/vsimem/out_write_nodata_only.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      self.driver.CreateCopy(
          tmpfilename, src_ds, options=['DATA_ENCODING=COMPLEX_PACKING']
      )
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      if datatype == gdal.GDT_Byte:
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 12)
        expected_vals = (12.0, 12.0, 12.0, 12.0)
      else:
        self.assertAlmostEqual(
            ds.GetRasterBand(1).GetNoDataValue(), 12.3, delta=12.3 * 1e-4
        )
        expected_vals = (12.3, 12.3, 12.3, 12.3)
      got_vals = struct.unpack(4 * 'd', ds.ReadRaster())
      ds = None
      for got, expected in zip(got_vals, expected_vals):
        self.assertAlmostEqual(got, expected, delta=abs(expected) * 1e-4)

  @parameterized.parameters([gdal.GDT_Byte, gdal.GDT_Float32])
  def test_grib_grib2_write_full_OneData(self, datatype: int):
    src_ds = self._createMemWgs84Dataset(2, 2, datatype)
    src_ds.GetRasterBand(1).SetNoDataValue(123)
    src_ds.WriteRaster(
        0,
        0,
        2,
        2,
        struct.pack(4 * 'f', 25.4, 25.4, 25.4, 25.4),
        buf_type=gdal.GDT_Float32,
    )
    tmpfilename = '/vsimem/out_write_full_onedata.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      self.driver.CreateCopy(
          tmpfilename, src_ds, options=['DATA_ENCODING=COMPLEX_PACKING']
      )
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 123)
      got_vals = struct.unpack(4 * 'd', ds.ReadRaster())
      ds = None
      if datatype == gdal.GDT_Byte:
        expected_vals = (25.0, 25.0, 25.0, 25.0)
      else:
        expected_vals = (25.4, 25.4, 25.4, 25.4)
      for got, expected in zip(got_vals, expected_vals):
        self.assertAlmostEqual(got, expected, delta=abs(expected) * 1e-4)

  def test_grib_grib2_write_mix_nodata_and_a_single_data(self):
    src_ds = gdal.Open(self.getTestFilePath('one_value_and_nodata_points.grb2'))
    self.assertIsNotNone(src_ds)
    tmpfilename = '/vsimem/out_mix_nodata.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      self.driver.CreateCopy(
          tmpfilename, src_ds, options=['DATA_ENCODING=COMPLEX_PACKING']
      )
      src_ds = None
      ds = gdal.Open(tmpfilename)
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 9999)
      got_vals = struct.unpack(400 * 'd', ds.ReadRaster())
      ds = None
      self.assertEqual(got_vals[0], 9999)
      self.assertAlmostEqual(got_vals[6], 0.01, delta=0.01 * 1e-4)

  # Not ported: test_grib_online_grib2_jpeg2000_single_line (requires network).

  def test_grib_grib2_derived_forecast_spread(self):
    ds = gdal.Open(self.getTestFilePath('template_4_12_spread.grb2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    band = ds.GetRasterBand(1)
    self.assertEqual(band.GetMetadataItem('GRIB_UNIT'), '[spread]')
    self.assertEqual(
        band.ComputeRasterMinMax(), (0.24296024441719055, 0.24296024441719055)
    )

    tmpfilename = '/vsimem/out_spread.grb2'
    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      out_ds = self.driver.CreateCopy(tmpfilename, ds)
      self.assertIsNotNone(out_ds)
      assert out_ds is not None
      band = out_ds.GetRasterBand(1)
      self.assertEqual(band.GetMetadataItem('GRIB_UNIT'), '[spread]')
      self.assertEqual(
          band.ComputeRasterMinMax(), (0.24296024441719055, 0.24296024441719055)
      )
      out_ds = None

  def test_grib_grib2_template_4_48(self):
    ds = gdal.Open(self.getTestFilePath('template_4_48.grb2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    band = ds.GetRasterBand(1)
    self.assertEqual(band.GetMetadataItem('GRIB_UNIT'), '[1/kg]')
    self.assertEqual(band.GetMetadataItem('GRIB_ELEMENT'), 'ASNCON')
    self.assertEqual(band.GetMetadataItem('GRIB_SHORT_NAME'), '0-EATM')

  def test_grib_grib2_scan_flag_not_64(self):
    ds = gdal.Open(
        '/vsisparse/'
        + self.getTestFilePath('blend.t17z.master.f001.co.grib2.sparse.xml')
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(),
        (-3272421.457337171, 2539.703, 0.0, 3790842.1060354356, 0.0, -2539.703),
        rel=1e-6,
    )

  @parameterized.parameters([True, False])
  def test_grib_grib2_read_subgrids(self, use_idx: bool):
    ds = gdal.OpenEx(
        self.getTestFilePath('subgrids.grib2'),
        open_options=['USE_IDX=YES' if use_idx else 'USE_IDX=NO'],
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    self.assertEqual(ds.GetRasterBand(2).Checksum(), 4563)
    expected_ids = (
        'CENTER=7(US-NCEP) SUBCENTER=0 MASTER_TABLE=2 LOCAL_TABLE=0'
        ' SIGNF_REF_TIME=1(Start_of_Forecast) REF_TIME=2020-09-26T00:00:00Z'
        ' PROD_STATUS=0(Operational) TYPE=1(Forecast)'
    )
    self.assertEqual(
        ds.GetRasterBand(1).GetMetadataItem('GRIB_IDS'), expected_ids
    )
    self.assertEqual(
        ds.GetRasterBand(1).GetMetadataItem('GRIB_COMMENT'),
        'u-component of wind [m/s]',
    )
    if use_idx:
      self.assertEqual(
          ds.GetRasterBand(1).GetDescription(),
          'UGRD:planetary boundary layer:anl',
      )
      self.assertEqual(
          ds.GetRasterBand(2).GetDescription(),
          'VGRD:planetary boundary layer:anl',
      )
    else:
      self.assertEqual(
          ds.GetRasterBand(1).GetDescription(),
          '0[-] RESERVED(220) (Reserved for local use)',
      )
      self.assertEqual(
          ds.GetRasterBand(2).GetDescription(),
          '0[-] RESERVED(220) (Reserved for local use)',
      )
    self.assertEqual(
        ds.GetRasterBand(2).GetMetadataItem('GRIB_IDS'), expected_ids
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetMetadataItem('GRIB_COMMENT'),
        'v-component of wind [m/s]',
    )
    self.assertEqual(
        ds.GetRasterBand(1).GetMetadataItem(
            'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES'
        ),
        '2 2 2 0 84 0 0 1 0 220 0 0 255 0 0',
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetMetadataItem(
            'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES'
        ),
        '2 3 2 0 84 0 0 1 0 220 0 0 255 0 0',
    )

  def test_grib_grib2_read_subgrids_reuse_bitmap(self):
    ds = gdal.Open(self.getTestFilePath('subgrids_reuse_bitmap.grib2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    self.assertEqual(ds.GetRasterBand(2).Checksum(), 4563)

  @parameterized.named_parameters(
      (
          'full_globe',
          'gfs.t06z.pgrb2.1p0.grib2',
          (-185.0, 10.0, 0.0, 90.125, 0.0, -10.0),
          7514,
      ),
      (
          'partial_across_am',
          'gfs.t06z.pgrb2.1p0.partial_across_am.grib2',
          (24.875, 10.0, 0.0, 90.125, 0.0, -10.0),
          5060,
      ),
      (
          'partial_east_of_am',
          'gfs.t06z.pgrb2.1p0.partial_east_of_am.grib2',
          (-60.125, 10.0, 0.0, 90.125, 0.0, -10.0),
          698,
      ),
      (
          'partial_west_of_am',
          'gfs.t06z.pgrb2.1p0.partial_west_of_am.grib2',
          (24.875, 10.0, 0.0, 90.125, 0.0, -10.0),
          601,
      ),
      (
          'gfswave_global',
          'gfswave-11.t00z.global.0p25.f000.grib2',
          (-180.125, 0.25, 0.0, 90.125, 0.0, -0.25),
          52492,
      ),
  )
  def test_grib_grib2_split_and_swap(
      self, filename: str, expected_gt: tuple[float, ...], band1csum: int
  ):
    tmpfilename = '/vsimem/out_split_and_swap.grb2'
    ds = gdal.Open(self.getTestFilePath(filename))
    self.assertIsNotNone(ds)
    assert ds is not None
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(), expected_gt, rel=1e-6
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), band1csum)

    with gcore_util.GdalUnlinkWhenDone(tmpfilename):
      out_ds = self.driver.CreateCopy(tmpfilename, ds)
      self.assertIsNotNone(out_ds)
      assert out_ds is not None
      self._assertGeoTransformAlmostEqual(
          out_ds.GetGeoTransform(), expected_gt, rel=1e-6
      )
      self.assertEqual(out_ds.GetRasterBand(1).Checksum(), band1csum)
      out_ds = None

  def test_grib_grib2_disable_split_and_swap(self):
    with gdaltest.config_option('GRIB_ADJUST_LONGITUDE_RANGE', 'NO'):
      tmpfilename = '/vsimem/out_disable_split_and_swap.grb2'
      expected_gt = (-0.125, 10.0, 0.0, 90.125, 0.0, -10.0)

      ds = gdal.Open(self.getTestFilePath('gfs.t06z.pgrb2.1p0.grib2'))
      self.assertIsNotNone(ds)
      assert ds is not None
      self._assertGeoTransformAlmostEqual(
          ds.GetGeoTransform(), expected_gt, rel=1e-6
      )
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 7674)

      with gcore_util.GdalUnlinkWhenDone(tmpfilename):
        out_ds = self.driver.CreateCopy(tmpfilename, ds)
        self.assertIsNotNone(out_ds)
        assert out_ds is not None
        self._assertGeoTransformAlmostEqual(
            out_ds.GetGeoTransform(), expected_gt, rel=1e-6
        )
        self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 7674)
        out_ds = None

  def test_grib_grib2_split_and_swap_offset_am(self):
    temp_filename = '/vsimem/temp_offset_am.grb2'
    out_filename = '/vsimem/out_offset_am.grb2'
    ds = gdal.Open(self.getTestFilePath('gfs.t06z.pgrb2.1p0.grib2'))
    self.assertIsNotNone(ds)
    assert ds is not None

    with (
        gcore_util.GdalUnlinkWhenDone(temp_filename),
        gcore_util.GdalUnlinkWhenDone(out_filename),
    ):
      temp_ds = self.driver.CreateCopy(temp_filename, ds)
      self.assertIsNotNone(temp_ds)
      assert temp_ds is not None
      temp_ds.SetGeoTransform((119.975, 10.0, 0.0, 90.125, 0.0, -10.0))

      out_ds = self.driver.CreateCopy(out_filename, temp_ds)
      self.assertIsNotNone(out_ds)
      assert out_ds is not None
      self._assertGeoTransformAlmostEqual(
          out_ds.GetGeoTransform(),
          (-185.0, 10.0, 0.0, 90.125, 0.0, -10.0),
          rel=1e-6,
      )
      out_ds = None
      temp_ds = None

  def test_grib_grib2_sidecar(self):
    filepath = self.getTestFilePath('gfs.t06z.pgrb2.10p0.f010.grib2')
    self._assertSidecarComparison(
        filepath,
        expected_count=6,
        band6_desc_idx='VGRD:planetary boundary layer:10 hour fcst',
        band6_desc_no_idx='0[-] RESERVED(220) (Reserved for local use)',
    )
    ds_idx = gdal.Open(filepath)
    self.assertIsNotNone(ds_idx)
    assert ds_idx is not None
    self.assertEqual(
        ds_idx.GetRasterBand(2).GetMetadataItem('GRIB_ELEMENT'), 'REFD'
    )
    self.assertEqual(
        ds_idx.GetRasterBand(3).GetMetadataItem('GRIB_ELEMENT'), 'REFC'
    )
    self.assertEqual(
        ds_idx.GetRasterBand(6).GetMetadataItem(
            'GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES'
        ),
        '2 3 2 0 96 0 0 1 10 220 0 0 255 0 0',
    )
    self.assertEqual(
        ds_idx.GetRasterBand(6).GetMetadataItem('GRIB_REF_TIME'), '1631944800'
    )
    self.assertEqual(
        ds_idx.GetRasterBand(6).GetMetadataItem('GRIB_VALID_TIME'), '1631980800'
    )
    self.assertEqual(
        ds_idx.GetRasterBand(6).GetMetadataItem('GRIB_FORECAST_SECONDS'),
        '36000',
    )
    self.assertEqual(ds_idx.GetRasterBand(1).Checksum(), 59985)
    self.assertEqual(ds_idx.GetRasterBand(2).Checksum(), 59986)
    self.assertEqual(ds_idx.GetRasterBand(6).Checksum(), 206)

  def test_grib_grib2_sidecar_vsisubfile(self):
    filepath = self.getTestFilePath('gfs.t06z.pgrb2.10p0.f010.grib2')

    ds = gdal.Open(f'/vsisubfile/0_5359,{filepath}')
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterCount, 1)
    self.assertEqual(
        ds.GetRasterBand(1).GetDescription(), 'REFD:1 hybrid level:10 hour fcst'
    )

    ds_ref = gdal.OpenEx(
        f'/vsisubfile/0_5359,{filepath}', open_options=['USE_IDX=NO']
    )
    self.assertIsNotNone(ds_ref)
    assert ds_ref is not None
    self.assertEqual(ds_ref.RasterCount, 1)
    self.assertEqual(
        ds_ref.GetRasterBand(1).GetDescription(), '1[-] HYBL="Hybrid level"'
    )
    self.assertEqual(
        ds.GetRasterBand(1).Checksum(), ds_ref.GetRasterBand(1).Checksum()
    )

    size = 16077 - 5359
    ds = gdal.Open(f'/vsisubfile/5359_{size},{filepath}')
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterCount, 2)
    self.assertEqual(
        ds.GetRasterBand(1).GetDescription(), 'REFD:2 hybrid level:10 hour fcst'
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetDescription(),
        'REFC:entire atmosphere:10 hour fcst',
    )

    ds_ref = gdal.OpenEx(
        f'/vsisubfile/5359_{size},{filepath}', open_options=['USE_IDX=NO']
    )
    self.assertIsNotNone(ds_ref)
    assert ds_ref is not None
    self.assertEqual(ds_ref.RasterCount, 2)
    self.assertEqual(
        ds_ref.GetRasterBand(1).GetDescription(), '2[-] HYBL="Hybrid level"'
    )
    self.assertEqual(
        ds.GetRasterBand(1).Checksum(), ds_ref.GetRasterBand(1).Checksum()
    )
    self.assertEqual(
        ds.GetRasterBand(2).Checksum(), ds_ref.GetRasterBand(2).Checksum()
    )

    ds = gdal.Open(f'/vsisubfile/16077_-1,{filepath}')
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterCount, 3)
    self.assertEqual(
        ds.GetRasterBand(1).GetDescription(), 'VIS:surface:10 hour fcst'
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetDescription(),
        'UGRD:planetary boundary layer:10 hour fcst',
    )
    self.assertEqual(
        ds.GetRasterBand(3).GetDescription(),
        'VGRD:planetary boundary layer:10 hour fcst',
    )

  def test_grib_grib1_2_mix_sidecar(self):
    filepath = self.getTestFilePath('broken_combined_grib2_grib1.grb2')
    self._assertSidecarComparison(
        filepath,
        expected_count=18,
        band6_desc_idx='VGRD:planetary boundary layer:10 hour fcst',
        band6_desc_no_idx='0[-] RESERVED(220) (Reserved for local use)',
    )
    ds_idx = gdal.Open(filepath)
    self.assertIsNotNone(ds_idx)
    assert ds_idx is not None
    self.assertEqual(
        ds_idx.GetRasterBand(18).GetDescription(),
        'DIRSW:Ground or water surface:anl',
    )
    self.assertEqual(
        ds_idx.GetRasterBand(2).GetMetadataItem('GRIB_ELEMENT'), 'REFD'
    )
    self.assertEqual(
        ds_idx.GetRasterBand(18).GetMetadataItem('GRIB_ELEMENT'), 'DIRSW'
    )
    self.assertEqual(ds_idx.GetRasterBand(1).Checksum(), 59985)
    self.assertEqual(ds_idx.GetRasterBand(18).Checksum(), 4794)

    ds_no_idx = gdal.OpenEx(
        filepath, gdal.GA_ReadOnly, open_options=['USE_IDX=NO']
    )
    self.assertIsNotNone(ds_no_idx)
    assert ds_no_idx is not None
    self.assertEqual(
        ds_no_idx.GetRasterBand(18).GetDescription(),
        '1[-] SFC (Ground or water surface)',
    )

  def test_grib_grib2_parameter_in_wmo_tables_only(self):
    ds = gdal.Open(self.getTestFilePath('parameter_in_wmo_tables_only.grb2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    band = ds.GetRasterBand(1)
    self.assertEqual(
        band.GetMetadataItem('GRIB_COMMENT'),
        'Latent heat net flux due to evaporation [W m-2]',
    )
    self.assertEqual(
        band.GetMetadataItem('GRIB_ELEMENT'),
        'Latent heat net flux due to evaporation',
    )

  def test_grib_grib1_south_polar_stereographic(self):
    ds = gdal.Open(
        '/vsisparse/' + self.getTestFilePath('south_polar_stereo_grib1.grb.xml')
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertIn(
        '+proj=stere +lat_0=-90 +lat_ts=-60',
        ds.GetSpatialRef().ExportToProj4(),
    )
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(),
        (-3243994.6063763676, 7673.0, 0.0, 3286668.2989108698, 0.0, -7673.0),
    )

  def test_grib_grib2_laea_negative_longitudes(self):
    ds = gdal.Open(
        self.getTestFilePath('laea_with_negative_longitudes_issue_7456.grib2')
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertIn(
        '+proj=laea +lat_0=54.9 +lon_0=-2.5 +x_0=0 +y_0=0',
        ds.GetSpatialRef().ExportToProj4(),
    )
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(),
        (-1158999.9595231502, 2000.0, 0.0, 903000.0029299166, 0.0, -2000.0),
    )

  def test_grib_grib2_wrong_earth_shape(self):
    filepath = self.getTestFilePath('byte_wrong_earth_shape.grib2')
    with gdal.quiet_errors():
      ds = gdal.Open(filepath)
      self.assertIn(
          'The GRIB file contains invalid values for the spheroid',
          gdal.GetLastErrorMsg(),
      )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertIsNone(ds.GetSpatialRef())

    with gdaltest.config_option('GRIB_USE_DEFAULT_SPHEROID', 'YES'):
      ds = gdal.Open(filepath)
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetSpatialRef().GetSemiMajor(), 6377563.396)
      self.assertEqual(ds.GetSpatialRef().GetSemiMinor(), 6377563.396)

  def test_grib_grib2_template_5_42_CCDS_aes_decompression(self):
    filepath = self.getTestFilePath('template_5_42_ccsds_aec.grb2')
    self.CheckOpen(filepath)
    self.assertTrue(self.src.GetDriver().GetMetadataItem('HAVE_AEC'))  # pyrefly: ignore[missing-attribute]
    self.CheckBand(1, 41970)

  def test_grib_grib2_minx_180(self):
    ds = gdal.Open(self.getTestFilePath('minx_180.grib2'))
    self.assertIsNotNone(ds)
    assert ds is not None
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(),
        (-180.0625, 0.125, 0.0, 90.0625, 0.0, -0.125),
        rel=1e-6,
    )

  def test_grib_grib2_MANAL_2023030103_fake_wrong_grid_origin_latitude(self):
    with gdal.quiet_errors():
      ds = gdal.Open(
          self.getTestFilePath(
              'MANAL_2023030103_fake_wrong_grid_origin_latitude.grb2'
          )
      )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(
        gdal.GetLastErrorMsg(),
        'Likely buggy grid registration for GRIB2 product: heuristics shows'
        ' that the latitudeOfFirstGridPoint is likely to qualify the latitude'
        ' of the northern-most grid point instead of the southern-most grid'
        ' point as expected. Please report to data producer. This heuristics'
        ' can be disabled by setting the'
        ' GRIB_LATITUDE_OF_FIRST_GRID_POINT_IS_SOUTHERN_MOST configuration'
        ' option to YES.',
    )
    self._assertGeoTransformAlmostEqual(
        ds.GetGeoTransform(),
        (-2442500.0217935005, 5000.0, 0.0, 2042500.0318467868, 0.0, -5000.0),
        rel=1e-6,
    )

  def test_grib_grib2_tmerc_negative_false_easting_false_northing(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 2, 2)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.SetProjection(
        '+proj=tmerc +lat_0=-1 +lon_0=-2 +k=1 +x_0=-300000 +y_0=-400000'
    )
    src_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    out_filename = '/vsimem/tmp_tmerc_neg.grb2'
    with gcore_util.GdalUnlinkWhenDone(out_filename):
      self.driver.CreateCopy(out_filename, src_ds)
      with gdal.Open(out_filename) as ds:
        self.assertIsNotNone(ds)
        assert ds is not None
        self.assertIn(
            '+proj=tmerc +lat_0=-1 +lon_0=-2 +k=1 +x_0=-300000 +y_0=-400000',
            ds.GetSpatialRef().ExportToProj4(),
        )


class TestReadGrib(gdrivers_util.DriverTestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)

  def getTestFilePath(self, filename: str) -> str:
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self):
    self.CheckDriver()

  def testInfo(self):
    globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
    for jsonfile in sorted(glob.glob(globpath)):
      filepath = os.path.splitext(jsonfile)[0]
      with self.subTest(input=os.path.basename(filepath)):
        self.CheckOpen(filepath)
        self.CheckInfo()


if __name__ == '__main__':
  googletest.main()
