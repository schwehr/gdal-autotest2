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
# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2009-2012, Even Rouault <even dot rouault at mines-paris.org>
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

"""Tests for the ENVI driver.

Format is described here:

https://gdal.org/drivers/raster/envi.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/envi.py
"""
from collections.abc import Iterator
import contextlib
import glob
import os
import struct

from osgeo import gdal
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.ENVI_DRIVER
EXT = '.dat'


@contextlib.contextmanager
def _set_cache_max(max_bytes: int) -> Iterator[None]:
  """Temporarily overrides GDAL's raster block cache limit."""
  old_cache_max = gdal.GetCacheMax()
  gdal.SetCacheMax(max_bytes)
  try:
    yield
  finally:
    gdal.SetCacheMax(old_cache_max)


class TestEnvi(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    gdal.DontUseExceptions()
    self._vsimem_dir = f'/vsimem/envi_test/{self._testMethodName}'
    self.addCleanup(gdal.RmdirRecursive, self._vsimem_dir)

  def get_test_file_path(self, filename: str) -> str:
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def _vsimem_path(self, filename: str) -> str:
    return f'{self._vsimem_dir}/{filename}'

  def _write_vsimem_envi(
      self, hdr_content: str, bin_data: bytes = b'xyz', stem: str = 'test'
  ) -> str:
    hdr_path = self._vsimem_path(f'{stem}.hdr')
    bin_path = self._vsimem_path(f'{stem}.bin')
    gdal.FileFromMemBuffer(hdr_path, hdr_content)
    gdal.FileFromMemBuffer(bin_path, bin_data)
    return bin_path

  def test_driver(self):
    self.CheckDriver()

  def test_info(self):
    globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
    for jsonfile in sorted(glob.glob(globpath)):
      filepath = os.path.splitext(jsonfile)[0]
      with self.subTest(input=os.path.basename(filepath)):
        self.CheckOpen(filepath)
        self.CheckInfo()

  def test_create_copy(self):
    self.CheckCreateCopySimple(gdrivers_util.TIFF_BYTE_FILE)

  # test_envi_1 done as an info test on aea.dat.json

  def test_envi_2(self):
    # Verify aea.dat can be exported losslessly
    filepath = self.get_test_file_path('aea.dat')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreateCopy(check_projection=False)

  def test_envi_3(self):
    # Try the Create interface with an RGB image.
    filepath = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')

    self.CheckOpen(filepath, check_driver=False)
    self.CheckCreate()

  def test_envi_4(self):
    # Test LCC Projection.
    self.CheckOpen(self.get_test_file_path('aea.dat'))
    prj = (
        'PROJCS["unnamed",'
        'GEOGCS["NAD83",'
        'DATUM["North_American_Datum_1983",'
        'SPHEROID["GRS 1980",6378137,298.257222101],'
        'TOWGS84[0,0,0,0,0,0,0]],'
        'PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]],'
        'PROJECTION["Lambert_Conformal_Conic_2SP"],'
        'PARAMETER["standard_parallel_1",33.90363402775256],'
        'PARAMETER["standard_parallel_2",33.62529002776137],'
        'PARAMETER["latitude_of_origin",33.76446202775696],'
        'PARAMETER["central_meridian",-117.4745428888127],'
        'PARAMETER["false_easting",20000],'
        'PARAMETER["false_northing",30000],'
        'UNIT["Meter",1]]'
    )
    self.CheckSetProjection(prj=prj)

  def test_envi_5(self):
    # Test TM Projection.
    self.CheckOpen(self.get_test_file_path('aea.dat'))
    prj = (
        'PROJCS["unnamed",'
        'GEOGCS["GCS_unnamed",'
        'DATUM["D_unnamed",'
        'SPHEROID["Airy 1830",6377563.396,299.3249646,'
        'AUTHORITY["EPSG","7001"]]],'
        'PRIMEM["Greenwich",0],'
        'UNIT["degree",0.01745329251994328]],'
        'PROJECTION["Transverse_Mercator"],'
        'PARAMETER["latitude_of_origin",49],'
        'PARAMETER["central_meridian",-2],'
        'PARAMETER["scale_factor",0.9996012717],'
        'PARAMETER["false_easting",400000],'
        'PARAMETER["false_northing",-100000],'
        'UNIT["metre",1,'
        'AUTHORITY["EPSG","9001"]],'
        'AXIS["Easting",EAST],'
        'AXIS["Northing",NORTH]]'
    )
    self.CheckSetProjection(prj=prj)

  def test_envi_6(self):
    # Test LAEA Projection.
    self.CheckOpen(self.get_test_file_path('aea.dat'))
    prj = (
        'PROJCS["unnamed",'
        'GEOGCS["Unknown datum based upon the Authalic Sphere",'
        'DATUM["D_Ellipse_Based",'
        'SPHEROID["Sphere",6370997,0]],'
        'PRIMEM["Greenwich",0],'
        'UNIT["Degree",0.0174532925199433]],'
        'PROJECTION["Lambert_Azimuthal_Equal_Area"],'
        'PARAMETER["latitude_of_center",33.764462027757],'
        'PARAMETER["longitude_of_center",-117.474542888813],'
        'PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],'
        'UNIT["metre",1,'
        'AUTHORITY["EPSG","9001"]],'
        'AXIS["Easting",EAST],'
        'AXIS["Northing",NORTH]]'
    )
    self.CheckSetProjection(prj=prj)

  def test_envi_7(self):
    # Verify VSIF*L capacity.
    self.CheckOpen(self.get_test_file_path('aea.dat'))
    self.CheckCreateCopy(
        check_projection=False, vsimem=True, remove_result=True
    )

  def test_envi_8(self):
    filepath = '/vsimem/foo.bsq'
    dataset = self.driver.Create(filepath, 10, 10, 1)
    expect_geotransform = (50000, 1, 0, 4500000, 0, -1)
    dataset.SetGeoTransform(expect_geotransform)
    self.assertEqual(expect_geotransform, dataset.GetGeoTransform())
    self.driver.Delete(filepath)

  def test_envi_9(self):
    # Verify reading and copying a compressed file.
    self.CheckOpen(self.get_test_file_path('aea_compressed.dat'))
    self.CheckCreateCopy(
        check_projection=False, vsimem=True, remove_result=True
    )

  def test_envi_10(self):
    filepath = self.get_test_file_path('envirpc.img')
    self.CheckOpen(filepath)
    dst_filepath = '/vsimem/envirpc.img'
    self.driver.CreateCopy(dst_filepath, self.src)

    gdal.Unlink(dst_filepath + '.aux.xml')

    dst = gdal.Open(dst_filepath)
    self.assertEqual('3355', dst.GetMetadata('RPC')['HEIGHT_OFF'])
    dst = None  # pylint: disable=unused-variable

    self.driver.Delete(dst_filepath)

  def test_envi_11(self):
    # Check .sta reading.
    ds = gdal.Open(self.get_test_file_path('envistat'))
    self.assertIsNotNone(ds)
    assert ds is not None
    val = ds.GetRasterBand(1).GetStatistics(0, 0)
    ds = None
    self.assertEqual(val, [1.0, 3.0, 2.0, 0.5])

  def test_envi_12(self):
    # Test reading and writing category names
    filepath = self.get_test_file_path('testenviclasses')
    self.CheckOpen(filepath)

    dst_filepath = '/vsimem/testenviclasses'
    dst = self.driver.CreateCopy(dst_filepath, self.src)
    band = dst.GetRasterBand(1)
    self.assertEqual(['Black', 'White'], band.GetCategoryNames())
    dst.FlushCache()
    del dst
    self.src = None

    gdal.Unlink(dst_filepath + '.aux.xml')

    self.CheckOpen(dst_filepath)
    assert self.src is not None
    band = self.src.GetRasterBand(1)
    self.assertEqual(['Black', 'White'], band.GetCategoryNames())
    color_table = band.GetColorTable()
    self.assertEqual(2, color_table.GetCount())
    self.assertEqual((0, 0, 0, 255), color_table.GetColorEntry(0))
    self.assertEqual((255, 255, 255, 255), color_table.GetColorEntry(1))

    self.driver.Delete(dst_filepath)

  def test_envi_13(self):
    # Writing of metadata from the ENVI metadata domain and read it back
    # https://trac.osgeo.org/gdal/ticket/4957
    filepath = '/vsimem/envi_13.dat'
    width = 1
    height = 2
    src = self.driver.Create(filepath, width, height)
    src.SetMetadata(['lines=100', 'sensor_type=Landsat TM', 'foo'], 'ENVI')
    src = None  # pylint: disable=unused-variable

    gdal.Unlink(filepath + '.aux.xml')

    src = gdal.Open(filepath)
    self.assertEqual(width, src.RasterXSize)
    self.assertEqual(height, src.RasterYSize)
    self.assertEqual('Landsat TM', src.GetMetadataItem('sensor_type', 'ENVI'))
    src = None  # pylint: disable=unused-variable

    self.driver.Delete(filepath)

  def test_envi_14(self):
    # Test that the image file is at the expected size on closing.
    # https://trac.osgeo.org/gdal/ticket/6662
    filepath = '/vsimem/envi_14.dat'
    self.driver.Create(filepath, 3, 4, 5, gdal.GDT_Int16)
    gdal.Unlink(filepath + '.aux.xml')
    expect = 3 * 4 * 5 * 2
    self.assertEqual(expect, gdal.VSIStatL(filepath).size)
    self.driver.Delete(filepath)

  def test_envi_15(self):
    # Writing geotransform matrix with rotation
    filepath = self.get_test_file_path('rotation.img')
    self.CheckOpen(filepath)
    # GeoTransform checked by the info test.
    self.CheckCreateCopy()

  def test_envi_truncated(self):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif'))
    self.assertIsNotNone(src_ds)
    assert src_ds is not None

    filepath = self._vsimem_path('envi_truncated.dat')
    dst_ds = self.driver.CreateCopy(filepath, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None
    src_ds = None

    fh = gdal.VSIFOpenL(filepath, 'rb+')
    self.assertIsNotNone(fh)
    gdal.VSIFTruncateL(fh, 20 * 20 // 2)
    gdal.VSIFCloseL(fh)

    with gdal.config_option('RAW_CHECK_FILE_SIZE', 'YES'):
      ds = gdal.Open(filepath)
    self.assertIsNotNone(ds)
    assert ds is not None
    cs = ds.GetRasterBand(1).Checksum()
    ds = None
    self.driver.Delete(filepath)

    self.assertEqual(cs, 2315)

  def test_envi_gcp(self):
    filepath = self._vsimem_path('test_envi_gcp.dat')
    ds = self.driver.Create(filepath, 1, 1)
    self.assertIsNotNone(ds)
    assert ds is not None
    gcp = gdal.GCP()
    gcp.GCPPixel = 1
    gcp.GCPLine = 2
    gcp.GCPX = 3
    gcp.GCPY = 4
    ds.SetGCPs([gcp], '')
    ds = None
    gdal.Unlink(filepath + '.aux.xml')

    ds = gdal.Open(filepath)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetGCPCount(), 1)
    gcps = ds.GetGCPs()
    self.assertLen(gcps, 1)
    got_gcp = gcps[0]
    ds = None

    self.assertEqual(got_gcp.GCPPixel, 1)
    self.assertEqual(got_gcp.GCPLine, 2)
    self.assertEqual(got_gcp.GCPX, 3)
    self.assertEqual(got_gcp.GCPY, 4)

    self.driver.Delete(filepath)

  def test_envi_bigendian(self):
    src_dat = self.get_test_file_path('uint16_envi_bigendian.dat')
    ds = gdal.Open(src_dat)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    ds = None

    for ext in ('dat', 'hdr'):
      name = f'uint16_envi_bigendian.{ext}'
      with open(self.get_test_file_path(name), 'rb') as f:
        gdal.FileFromMemBuffer(self._vsimem_path(name), f.read())

    filepath = self._vsimem_path('uint16_envi_bigendian.dat')
    ds = gdal.Open(filepath, gdal.GA_Update)
    self.assertIsNotNone(ds)
    assert ds is not None
    ds.SetGeoTransform([0, 2, 0, 0, 0, -2])
    ds = None

    ds = gdal.Open(filepath)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    ds = None

    self.driver.Delete(filepath)

  @parameterized.parameters(
      ('bip', 'PIXEL'),
      ('bil', 'LINE'),
      ('bsq', 'BAND'),
  )
  def test_envi_interleaving(
      self, filename_suffix: str, expected_interleave: str
  ):
    filepath = self.get_test_file_path(f'envi_rgbsmall_{filename_suffix}.img')
    ds = gdal.Open(filepath)
    self.assertIsNotNone(ds, filepath)
    assert ds is not None
    self.assertEqual(
        ds.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'),
        expected_interleave,
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 20718)
    self.assertEqual(ds.GetRasterBand(2).Checksum(), 20669)
    self.assertEqual(ds.GetRasterBand(3).Checksum(), 20895)
    ds = None

  def test_envi_nodata(self):
    filepath = self._vsimem_path('test_envi_nodata.dat')
    ds = self.driver.Create(filepath, 1, 1)
    self.assertIsNotNone(ds)
    assert ds is not None
    ds.GetRasterBand(1).SetNoDataValue(1)
    ds = None

    gdal.Unlink(filepath + '.aux.xml')

    ds = gdal.Open(filepath)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 1.0)
    ds = None

    self.driver.Delete(filepath)

  def test_envi_rotation_180(self):
    filepath = self._vsimem_path('test_envi_rotation_180.dat')
    ds = self.driver.Create(filepath, 1, 1)
    self.assertIsNotNone(ds)
    assert ds is not None
    ds.SetGeoTransform([0, 10, 0, 0, 0, 10])
    ds = None

    gdal.Unlink(filepath + '.aux.xml')

    ds = gdal.Open(filepath)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetGeoTransform(), (0, 10, 0, 0, 0, 10))
    ds = None

    self.driver.Delete(filepath)

  @parameterized.parameters(
      ('bip', True),
      ('bip', False),
      ('bil', True),
      ('bil', False),
      ('bsq', True),
      ('bsq', False),
  )
  def test_envi_writing_interleaving(self, interleaving: str, explicit: bool):
    src_filename = self.get_test_file_path(f'envi_rgbsmall_{interleaving}.img')
    dst_filename = self._vsimem_path('out')
    creation_options = [f'INTERLEAVE={interleaving}'] if explicit else []
    try:
      out_ds = gdal.Translate(
          dst_filename,
          src_filename,
          format='ENVI',
          creationOptions=creation_options,
      )
      self.assertIsNotNone(out_ds)
      out_ds = None

      with open(src_filename, 'rb') as f:
        ref_data = f.read()
      got_data = gcore_util.vsi_read_file_as_bytes(
          dst_filename, len(ref_data) + 100
      )
      self.assertEqual(got_data, ref_data)
    finally:
      gdal.Unlink(dst_filename)
      gdal.Unlink(dst_filename + '.hdr')

  @parameterized.parameters('bip', 'bil', 'bsq')
  def test_envi_writing_interleaving_larger_file(self, interleaving: str):
    dst_filename = self._vsimem_path('out')
    xsize = 10000
    ysize = 10
    bands = 100
    try:
      with _set_cache_max(xsize * (ysize // 2)):
        ds = self.driver.Create(
            dst_filename,
            xsize,
            ysize,
            bands,
            options=[f'INTERLEAVE={interleaving}'],
        )
        self.assertIsNotNone(ds)
        assert ds is not None
        ds.GetRasterBand(1).Fill(1)
        for i in range(bands):
          val = struct.pack('B', i + 1)
          ds.GetRasterBand(i + 1).WriteRaster(
              0, 0, xsize, ysize // 2, val * (xsize * (ysize // 2))
          )
        for i in range(bands):
          val = struct.pack('B', i + 1)
          ds.GetRasterBand(i + 1).WriteRaster(
              0, ysize // 2, xsize, ysize // 2, val * (xsize * (ysize // 2))
          )
        ds = None

      ds = gdal.Open(dst_filename)
      self.assertIsNotNone(ds)
      assert ds is not None
      for i in range(bands):
        val = struct.pack('B', i + 1)
        self.assertEqual(
            ds.GetRasterBand(i + 1).ReadRaster(), val * (xsize * ysize)
        )
      ds = None
    finally:
      gdal.Unlink(dst_filename)
      gdal.Unlink(dst_filename + '.hdr')

  def test_envi_add_hdr(self):
    path_int = self._vsimem_path('test.int')
    ds = self.driver.Create(
        path_int,
        xsize=10,
        ysize=10,
        bands=1,
        eType=gdal.GDT_CFloat32,
        options=['SUFFIX=ADD'],
    )
    self.assertIsNotNone(ds)
    ds = None

    ds = gdal.Open(path_int)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterCount, 1)
    ds = None

    path_mph = self._vsimem_path('test.int.mph')
    ds = self.driver.Create(
        path_mph,
        xsize=10,
        ysize=10,
        bands=2,
        eType=gdal.GDT_Float32,
        options=['SUFFIX=ADD'],
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    # Verify test.int.mph.hdr takes precedence over test.int.hdr.
    self.assertEqual(ds.RasterCount, 2)
    ds = None

    ds = gdal.Open(path_mph)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterCount, 2)
    ds = None

    self.driver.Delete(path_int)
    self.driver.Delete(path_mph)

  def test_envi_edit_coordinate_system_string(self):
    filename = self._vsimem_path('test.bin')
    ds = self.driver.Create(filename, 1, 1)
    self.assertIsNotNone(ds)
    assert ds is not None
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    ds.SetSpatialRef(srs)
    ds = None

    ds = gdal.Open(filename, gdal.GA_Update)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), '4326')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(3261)
    ds.SetSpatialRef(srs)
    ds = None

    content = gcore_util.vsi_read_file_as_string(filename[:-4] + '.hdr', 4096)
    self.assertEqual(content.count('coordinate system string'), 1)

    ds = gdal.Open(filename)
    self.assertIsNotNone(ds)
    assert ds is not None
    srs = ds.GetSpatialRef()
    self.assertTrue(srs.IsProjected())
    self.assertEqual(srs.GetAuthorityCode(None), '3261')
    ds = None

    self.driver.Delete(filename)

  def test_envi_read_default_bands_rgb(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'samples = 1\n'
        'lines = 1\n'
        'bands = 3\n'
        'header offset = 0\n'
        'file type = ENVI Standard\n'
        'data type = 1\n'
        'interleave = bip\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'default bands = {3, 2, 1}'
    )

    ds = gdal.Open(bin_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(
        ds.GetRasterBand(1).GetColorInterpretation(), gdal.GCI_BlueBand
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetColorInterpretation(), gdal.GCI_GreenBand
    )
    self.assertEqual(
        ds.GetRasterBand(3).GetColorInterpretation(), gdal.GCI_RedBand
    )
    ds = None
    self.assertIsNone(gdal.VSIStatL(bin_path + '.aux.xml'))
    self.driver.Delete(bin_path)

  def test_envi_read_default_bands_gray(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'samples = 1\n'
        'lines = 1\n'
        'bands = 3\n'
        'header offset = 0\n'
        'file type = ENVI Standard\n'
        'data type = 1\n'
        'interleave = bip\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'default bands = {2}'
    )

    ds = gdal.Open(bin_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(
        ds.GetRasterBand(1).GetColorInterpretation(), gdal.GCI_Undefined
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetColorInterpretation(), gdal.GCI_GrayIndex
    )
    self.assertEqual(
        ds.GetRasterBand(3).GetColorInterpretation(), gdal.GCI_Undefined
    )
    ds = None
    self.assertIsNone(gdal.VSIStatL(bin_path + '.aux.xml'))
    self.driver.Delete(bin_path)

  def test_envi_write_default_bands_rgb(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1, 3)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_BlueBand)
    src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_RedBand)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_GreenBand)

    bin_path = self._vsimem_path('test.bin')
    dst_ds = self.driver.CreateCopy(bin_path, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None

    content = gcore_util.vsi_read_file_as_string(
        self._vsimem_path('test.hdr'), 4096
    )
    self.driver.Delete(bin_path)
    self.assertIn('default bands = {2, 3, 1}', content)

  def test_envi_write_default_bands_gray(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1, 3)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_Undefined)
    src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_GrayIndex)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_Undefined)

    bin_path = self._vsimem_path('test.bin')
    dst_ds = self.driver.CreateCopy(bin_path, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None

    content = gcore_util.vsi_read_file_as_string(
        self._vsimem_path('test.hdr'), 4096
    )
    self.driver.Delete(bin_path)
    self.assertIn('default bands = {2}', content)

  def test_envi_write_default_bands_duplicate_color_rgb(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1, 6)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_BlueBand)
    src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_RedBand)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_GreenBand)
    src_ds.GetRasterBand(4).SetColorInterpretation(gdal.GCI_BlueBand)
    src_ds.GetRasterBand(5).SetColorInterpretation(gdal.GCI_RedBand)
    src_ds.GetRasterBand(6).SetColorInterpretation(gdal.GCI_GreenBand)

    bin_path = self._vsimem_path('test.bin')
    dst_ds = self.driver.CreateCopy(bin_path, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None

    content = gcore_util.vsi_read_file_as_string(
        self._vsimem_path('test.hdr'), 4096
    )
    self.driver.Delete(bin_path)
    self.assertNotIn('default bands', content)

  def test_envi_write_default_bands_duplicate_color_gray(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1, 6)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_GrayIndex)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_GrayIndex)

    bin_path = self._vsimem_path('test.bin')
    dst_ds = self.driver.CreateCopy(bin_path, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None

    content = gcore_util.vsi_read_file_as_string(
        self._vsimem_path('test.hdr'), 4096
    )
    self.driver.Delete(bin_path)
    self.assertNotIn('default bands', content)

  def test_envi_read_data_offset_values(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'samples = 1\n'
        'lines = 1\n'
        'bands = 3\n'
        'header offset = 0\n'
        'file type = ENVI Standard\n'
        'data type = 1\n'
        'interleave = bip\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'data offset values = {3.5,2,1}'
    )

    ds = gdal.Open(bin_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).GetOffset(), 3.5)
    self.assertEqual(ds.GetRasterBand(2).GetOffset(), 2)
    self.assertEqual(ds.GetRasterBand(3).GetOffset(), 1)
    ds = None
    self.assertIsNone(gdal.VSIStatL(bin_path + '.aux.xml'))
    self.driver.Delete(bin_path)

  def test_envi_read_data_gain_values(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'samples = 1\n'
        'lines = 1\n'
        'bands = 3\n'
        'header offset = 0\n'
        'file type = ENVI Standard\n'
        'data type = 1\n'
        'interleave = bip\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'data gain values = {3.5,2,1}'
    )

    ds = gdal.Open(bin_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).GetScale(), 3.5)
    self.assertEqual(ds.GetRasterBand(2).GetScale(), 2)
    self.assertEqual(ds.GetRasterBand(3).GetScale(), 1)
    ds = None
    self.assertIsNone(gdal.VSIStatL(bin_path + '.aux.xml'))
    self.driver.Delete(bin_path)

  def test_envi_write_data_offset_values(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1, 3)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.GetRasterBand(2).SetOffset(10)

    bin_path = self._vsimem_path('test.bin')
    dst_ds = self.driver.CreateCopy(bin_path, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None

    content = gcore_util.vsi_read_file_as_string(
        self._vsimem_path('test.hdr'), 4096
    )
    self.driver.Delete(bin_path)
    self.assertIn('data offset values = {0, 10, 0}', content)

  def test_envi_write_data_gain_values(self):
    src_ds = gdal.GetDriverByName('MEM').Create('', 1, 1, 3)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None
    src_ds.GetRasterBand(2).SetScale(10)

    bin_path = self._vsimem_path('test.bin')
    dst_ds = self.driver.CreateCopy(bin_path, src_ds)
    self.assertIsNotNone(dst_ds)
    dst_ds = None

    content = gcore_util.vsi_read_file_as_string(
        self._vsimem_path('test.hdr'), 4096
    )
    self.driver.Delete(bin_path)
    self.assertIn('data gain values = {1, 10, 1}', content)

  @parameterized.parameters('LITTLE_ENDIAN', 'BIG_ENDIAN')
  def test_envi_read_direct_access(self, byte_order: str):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif'))
    self.assertIsNotNone(src_ds)
    assert src_ds is not None

    filename = self._vsimem_path('test.bin')
    out_ds = gdal.Translate(
        filename,
        src_ds,
        format='ENVI',
        outputType=gdal.GDT_UInt16,
        creationOptions=['INTERLEAVE=BIP', f'@BYTE_ORDER={byte_order}'],
    )
    self.assertIsNotNone(out_ds)
    out_ds = None

    ds = gdal.Open(filename)
    self.assertIsNotNone(ds)
    assert ds is not None

    # Optimized direct access path.
    self.assertEqual(
        ds.ReadRaster(
            0,
            0,
            ds.RasterXSize,
            ds.RasterYSize,
            buf_type=gdal.GDT_UInt16,
            buf_pixel_space=2 * ds.RasterCount,
            buf_band_space=2,
        ),
        src_ds.ReadRaster(
            0,
            0,
            ds.RasterXSize,
            ds.RasterYSize,
            buf_type=gdal.GDT_UInt16,
            buf_pixel_space=2 * ds.RasterCount,
            buf_band_space=2,
        ),
    )

    self.assertEqual(
        ds.ReadRaster(
            1,
            2,
            3,
            4,
            buf_type=gdal.GDT_UInt16,
            buf_pixel_space=2 * ds.RasterCount,
            buf_band_space=2,
        ),
        src_ds.ReadRaster(
            1,
            2,
            3,
            4,
            buf_type=gdal.GDT_UInt16,
            buf_pixel_space=2 * ds.RasterCount,
            buf_band_space=2,
        ),
    )

    # Non-optimized path: buffer type != native data type.
    self.assertEqual(
        ds.ReadRaster(
            1,
            2,
            3,
            4,
            buf_type=gdal.GDT_Byte,
            buf_pixel_space=ds.RasterCount,
            buf_band_space=1,
        ),
        src_ds.ReadRaster(
            1,
            2,
            3,
            4,
            buf_type=gdal.GDT_Byte,
            buf_pixel_space=ds.RasterCount,
            buf_band_space=1,
        ),
    )

    ds = None
    self.driver.Delete(filename)

  def test_envi_read_direct_access_update_scenario(self):
    src_ds = gdal.Open(gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif'))
    self.assertIsNotNone(src_ds)
    assert src_ds is not None

    filename = self._vsimem_path('test.bin')
    ds = self.driver.Create(
        filename,
        src_ds.RasterXSize,
        src_ds.RasterYSize,
        src_ds.RasterCount,
        options=['INTERLEAVE=BIP'],
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, src_ds.ReadRaster())

    self.assertEqual(
        ds.ReadRaster(
            0,
            0,
            ds.RasterXSize,
            ds.RasterYSize,
            buf_type=gdal.GDT_Byte,
            buf_pixel_space=ds.RasterCount,
            buf_band_space=1,
        ),
        src_ds.ReadRaster(
            0,
            0,
            ds.RasterXSize,
            ds.RasterYSize,
            buf_type=gdal.GDT_Byte,
            buf_pixel_space=ds.RasterCount,
            buf_band_space=1,
        ),
    )

    ds = None
    self.driver.Delete(filename)

  @parameterized.parameters(
      (1, 1, False),
      (float('nan'), float('nan'), False),
      (1, 2, True),
      (float('nan'), 1, True),
      (1, float('nan'), True),
  )
  def test_envi_write_warn_different_nodata(
      self, nd1: float, nd2: float, expected_warning: bool
  ):
    filename = self._vsimem_path('warn_nodata.img')
    ds = self.driver.Create(filename, 1, 1, 2)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).SetNoDataValue(nd1), gdal.CE_None)
    gdal.ErrorReset()
    with gdal.quiet_errors():
      self.assertEqual(ds.GetRasterBand(2).SetNoDataValue(nd2), gdal.CE_None)
      self.assertEqual(
          gdal.GetLastErrorType(),
          gdal.CE_Warning if expected_warning else gdal.CE_None,
      )
    ds = None
    self.driver.Delete(filename)

  def test_envi_read_metadata_with_leading_space(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'samples = 1\n'
        'lines = 1\n'
        'bands = 3\n'
        'header offset = 0\n'
        'file type = ENVI Standard\n'
        'data type = 1\n'
        'interleave = bip\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        ' wavelength = {3, 2, 1}'
    )

    ds = gdal.Open(bin_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.GetRasterBand(1).GetMetadataItem('wavelength'), '3')
    ds = None
    self.driver.Delete(bin_path)

  @parameterized.named_parameters(
      ('um', 'um', '{3, 2, 1}', '{.3, .2, .1}'),
      ('nm', 'nm', '{3000, 2000, 1000}', '{300, 200, 100}'),
      ('mm', 'mm', '{0.003, 0.002, 0.001}', '{0.0003, 0.0002, 0.0001}'),
  )
  def test_envi_read_wavelength_fwhm(
      self, units: str, wavelength: str, fwhm: str
  ):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'samples = 1\n'
        'lines = 1\n'
        'bands = 3\n'
        'header offset = 0\n'
        'file type = ENVI Standard\n'
        'data type = 1\n'
        'interleave = bip\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        f'wavelength units = {units}\n'
        f'wavelength = {wavelength}\n'
        f'fwhm = {fwhm}'
    )

    ds = gdal.Open(bin_path)
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(
        ds.GetRasterBand(1).GetMetadataItem('CENTRAL_WAVELENGTH_UM', 'IMAGERY'),
        '3.000',
    )
    self.assertEqual(
        ds.GetRasterBand(1).GetMetadataItem('FWHM_UM', 'IMAGERY'), '0.300'
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetMetadataItem('CENTRAL_WAVELENGTH_UM', 'IMAGERY'),
        '2.000',
    )
    self.assertEqual(
        ds.GetRasterBand(2).GetMetadataItem('FWHM_UM', 'IMAGERY'), '0.200'
    )
    self.assertEqual(
        ds.GetRasterBand(3).GetMetadataItem('CENTRAL_WAVELENGTH_UM', 'IMAGERY'),
        '1.000',
    )
    self.assertEqual(
        ds.GetRasterBand(3).GetMetadataItem('FWHM_UM', 'IMAGERY'), '0.100'
    )
    ds = None
    self.driver.Delete(bin_path)

  def test_envi_read_too_large_lines(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'file type = ENVI Standard\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'header offset = 0\n'
        'samples = 2\n'
        'lines = 2147483648\n'
        'bands = 1\n'
        'x start = 1\n'
        'y start = 1\n'
        'interleave = bip\n'
    )

    gdal.ErrorReset()
    with gdal.quiet_errors():
      ds = gdal.Open(bin_path)
    self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
    self.assertIn(
        'Limiting number of lines from 2147483648 to 2147483647 due to GDAL'
        ' raster data model limitation',
        gdal.GetLastErrorMsg(),
    )
    self.assertIsNotNone(ds)
    assert ds is not None
    self.assertEqual(ds.RasterXSize, 2)
    self.assertEqual(ds.RasterYSize, 2147483647)
    ds = None

  def test_envi_read_too_large_samples(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'file type = ENVI Standard\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'header offset = 0\n'
        'samples = 2147483648\n'
        'lines = 2\n'
        'bands = 1\n'
        'x start = 1\n'
        'y start = 1\n'
        'interleave = bip\n'
    )

    with (
        gdal.ExceptionMgr(useExceptions=True),
        gdal.quiet_errors(),
        self.assertRaisesRegex(
            RuntimeError,
            'Cannot handle samples=2147483648 due to GDAL raster data model'
            ' limitation',
        ),
    ):
      gdal.Open(bin_path)

  def test_envi_read_too_large_bands(self):
    bin_path = self._write_vsimem_envi(
        'ENVI\n'
        'file type = ENVI Standard\n'
        'sensor type = Unknown\n'
        'byte order = 0\n'
        'header offset = 0\n'
        'samples = 1\n'
        'lines = 2\n'
        'bands = 2147483648\n'
        'x start = 1\n'
        'y start = 1\n'
        'interleave = bip\n'
    )

    with (
        gdal.ExceptionMgr(useExceptions=True),
        gdal.quiet_errors(),
        self.assertRaisesRegex(
            RuntimeError,
            'Cannot handle bands=2147483648 due to GDAL raster data model'
            ' limitation',
        ),
    ):
      gdal.Open(bin_path)


if __name__ == '__main__':
  googletest.main()
