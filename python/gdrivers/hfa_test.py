# Copyright 2014 Google Inc. All Rights Reserved.
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
# Copyright (c) 2003-2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2011, Even Rouault <even dot rouault at spatialys.com>
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

"""Tests for the HFA (Erdas Imagine .img) raster driver.

Format is described here:

https://gdal.org/drivers/raster/hfa.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/ea75dfb98cb6a6fe27d826f840178e8e6b6fe626/autotest/gdrivers/hfa.py
"""

import datetime
import glob
import math
import os
import shutil
import struct

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

EXT = '.img'
DRIVER = gdrivers_util.HFA_DRIVER

_UTMSMALL_HISTOBINVALUES = (
    '0|0|0|0|0|0|0|0|8|0|0|0|0|0|0|0|23|0|0|0|0|0|0|0|0|29|0|0|0|0|0|0|0|46|'
    '0|0|0|0|0|0|0|69|0|0|0|0|0|0|0|99|0|0|0|0|0|0|0|0|120|0|0|0|0|0|0|0|178|'
    '0|0|0|0|0|0|0|193|0|0|0|0|0|0|0|212|0|0|0|0|0|0|0|281|0|0|0|0|0|0|0|0|365|'
    '0|0|0|0|0|0|0|460|0|0|0|0|0|0|0|533|0|0|0|0|0|0|0|544|0|0|0|0|0|0|0|0|626|'
    '0|0|0|0|0|0|0|653|0|0|0|0|0|0|0|673|0|0|0|0|0|0|0|629|0|0|0|0|0|0|0|0|586|'
    '0|0|0|0|0|0|0|541|0|0|0|0|0|0|0|435|0|0|0|0|0|0|0|348|0|0|0|0|0|0|0|341|0|'
    '0|0|0|0|0|0|0|284|0|0|0|0|0|0|0|225|0|0|0|0|0|0|0|237|0|0|0|0|0|0|0|172|0|'
    '0|0|0|0|0|0|0|159|0|0|0|0|0|0|0|105|0|0|0|0|0|0|0|824|'
)

_WKT_87TEST = (
    'PROJCS["World_Cube",GEOGCS["WGS 84",DATUM["WGS_1984",'
    'SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],'
    'AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0],'
    'UNIT["Degree",0.0174532925199433]],PROJECTION["Cube"],'
    'PARAMETER["False_Easting",0],PARAMETER["False_Northing",0],'
    'PARAMETER["Central_Meridian",0],PARAMETER["Option",1],'
    'UNIT["metre",1,AUTHORITY["EPSG","9001"]],'
    'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
)


class HfaTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self) -> None:  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)

  def get_test_file_path(self, filename: str) -> str:
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def test_driver_open_and_band(self) -> None:
    self.CheckDriver()
    band_num = 1
    tests = (
        ('byte.img', 4672, gdal.GDT_Byte),
        ('int16.img', 4672, gdal.GDT_Int16),
        ('uint16.img', 4672, gdal.GDT_UInt16),
        ('int32.img', 4672, gdal.GDT_Int32),
        ('uint32.img', 4672, gdal.GDT_UInt32),
        ('float32.img', 4672, gdal.GDT_Float32),
        ('float64.img', 4672, gdal.GDT_Float64),
        ('utmsmall.img', 50054, gdal.GDT_Byte),
        ('2bit_compressed.img', 11918, gdal.GDT_Byte),
    )
    for filename, checksum, band_type in tests:
      filepath = self.get_test_file_path(filename)
      self.CheckOpen(filepath)
      self.CheckBand(band_num, checksum, band_type)

  def test_info(self) -> None:
    globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
    for jsonfile in sorted(glob.glob(globpath)):
      filepath = os.path.splitext(jsonfile)[0]
      with self.subTest(input=os.path.basename(filepath)):
        self.CheckOpen(filepath)
        self.CheckInfo()

  def test_hfa_histread(self) -> None:
    with gdal.Open(self.get_test_file_path('utmsmall.img')) as ds:
      md = ds.GetRasterBand(1).GetMetadata()

    self.assertEqual(md['STATISTICS_MINIMUM'], '8')
    self.assertEqual(md['STATISTICS_MEDIAN'], '148')
    self.assertEqual(md['STATISTICS_HISTOMAX'], '255')
    self.assertEqual(md['STATISTICS_HISTOBINVALUES'], _UTMSMALL_HISTOBINVALUES)
    self.assertEqual(md['STATISTICS_SKIPFACTORX'], '1')
    self.assertEqual(md['STATISTICS_SKIPFACTORY'], '1')
    self.assertEqual(md['STATISTICS_EXCLUDEDVALUES'], '0')

  def test_hfa_histwrite(self) -> None:
    filepath = self.get_test_file_path('utmsmall.img')
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_histwrite') as vsimem_dir:
      work_img = f'{vsimem_dir}/work.img'
      with gdal.Open(filepath) as ds_src:
        self.driver.CreateCopy(work_img, ds_src)

      gdal.Unlink(f'{work_img}.aux.xml')

      with gdal.Open(work_img) as ds:
        md = ds.GetRasterBand(1).GetMetadata()

      self.driver.Delete(work_img)

    self.assertEqual(md['STATISTICS_MINIMUM'], '8')
    self.assertEqual(md['STATISTICS_MEDIAN'], '148')
    self.assertEqual(md['STATISTICS_HISTOMAX'], '255')
    self.assertEqual(md['STATISTICS_HISTOBINVALUES'], _UTMSMALL_HISTOBINVALUES)

  def test_hfa_histrewrite(self) -> None:
    new_hist = (
        '8|23|29|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|'
        '46|0|0|0|0|0|0|0|69|0|0|0|0|0|0|0|99|0|0|0|0|0|0|0|0|120|0|0|0|0|0|0|0|'
        '178|0|0|0|0|0|0|0|193|0|0|0|0|0|0|0|212|0|0|0|0|0|0|0|281|0|0|0|0|0|0|'
        '0|0|365|0|0|0|0|0|0|0|460|0|0|0|0|0|0|0|533|0|0|0|0|0|0|0|544|0|0|0|0|'
        '0|0|0|0|626|0|0|0|0|0|0|0|653|0|0|0|0|0|0|0|673|0|0|0|0|0|0|0|629|0|0|'
        '0|0|0|0|0|0|586|0|0|0|0|0|0|0|541|0|0|0|0|0|0|0|435|0|0|0|0|0|0|0|348|'
        '0|0|0|0|0|0|0|341|0|0|0|0|0|0|0|0|284|0|0|0|0|0|0|0|225|0|0|0|0|0|0|0|'
        '237|0|0|0|0|0|0|0|172|0|0|0|0|0|0|0|0|159|0|0|0|0|0|0|0|105|0|0|0|0|0|'
        '0|0|824|'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_histrewrite') as vsimem_dir:
      work_img = f'{vsimem_dir}/work.img'
      with gdal.Open(self.get_test_file_path('utmsmall.img')) as ds_src:
        self.driver.CreateCopy(work_img, ds_src)

      gdal.Unlink(f'{work_img}.aux.xml')

      with gdal.Open(work_img, gdal.GA_Update) as ds:
        ds.GetRasterBand(1).SetMetadataItem(
            'STATISTICS_HISTOBINVALUES', new_hist
        )

      gdal.Unlink(f'{work_img}.aux.xml')

      with gdal.Open(work_img) as ds:
        hist_str = ds.GetRasterBand(1).GetMetadataItem(
            'STATISTICS_HISTOBINVALUES'
        )

      self.driver.Delete(work_img)

    self.assertEqual(hist_str, new_hist)

  def test_hfa_int_stats_1(self) -> None:
    with gdal.Open(self.get_test_file_path('int.img')) as ds:
      md = ds.GetRasterBand(1).GetMetadata()

    self.assertEqual(md['STATISTICS_MINIMUM'], '40918')
    self.assertEqual(md['STATISTICS_MAXIMUM'], '41134')
    self.assertEqual(md['STATISTICS_MEDIAN'], '41017')
    self.assertEqual(md['STATISTICS_MODE'], '41013')
    self.assertEqual(md['STATISTICS_HISTOMIN'], '40918')
    self.assertEqual(md['STATISTICS_HISTOMAX'], '41134')
    self.assertEqual(md['LAYER_TYPE'], 'athematic')

  def test_hfa_int_stats_2(self) -> None:
    with gdal.Open(self.get_test_file_path('int.img')) as ds:
      stats = ds.GetRasterBand(1).GetStatistics(False, True)

    self.assertIterAlmostEqual(
        stats,
        (40918.0, 41134.0, 41019.784218148, 44.637237445468),
        delta=0.0001,
    )

  def test_hfa_float_stats_1(self) -> None:
    with gdal.Open(self.get_test_file_path('float.img')) as ds:
      md = ds.GetRasterBand(1).GetMetadata()

    tolerance = 0.0001
    self.assertAlmostEqual(
        float(md['STATISTICS_MINIMUM']), 40.91858291626, delta=tolerance
    )
    self.assertAlmostEqual(
        float(md['STATISTICS_MAXIMUM']), 41.134323120117, delta=tolerance
    )
    self.assertAlmostEqual(
        float(md['STATISTICS_MEDIAN']), 41.017182931304, delta=tolerance
    )
    self.assertAlmostEqual(
        float(md['STATISTICS_MODE']), 41.0104410499, delta=tolerance
    )
    self.assertAlmostEqual(
        float(md['STATISTICS_HISTOMIN']), 40.91858291626, delta=tolerance
    )
    self.assertAlmostEqual(
        float(md['STATISTICS_HISTOMAX']), 41.134323120117, delta=tolerance
    )
    self.assertEqual(md['LAYER_TYPE'], 'athematic')

  def test_hfa_float_stats_2(self) -> None:
    with gdal.Open(self.get_test_file_path('float.img')) as ds:
      stats = ds.GetRasterBand(1).GetStatistics(False, True)

    self.assertIterAlmostEqual(
        stats,
        (40.91858291626, 41.134323120117, 41.020284249223, 0.044636441749041),
        delta=0.0001,
    )

  def test_hfa_int_read(self) -> None:
    with gdal.Open(self.get_test_file_path('int.img')) as ds:
      band = ds.GetRasterBand(1)
      cs = band.Checksum()
      pixel = band.ReadRaster(100, 100, 1, 1)

    self.assertEqual(cs, 6691)
    self.assertIsNotNone(pixel)

  def test_hfa_float_read(self) -> None:
    with gdal.Open(self.get_test_file_path('float.img')) as ds:
      band = ds.GetRasterBand(1)
      cs = band.Checksum()
      data = band.ReadRaster(100, 100, 1, 1)

    self.assertEqual(cs, 23529)
    self.assertIsNotNone(data)
    value = struct.unpack('f', data)[0]
    self.assertAlmostEqual(value, 41.021659851074219, delta=0.0001)

  def test_hfa_pe_read(self) -> None:
    with gdal.Open(self.get_test_file_path('87test.img')) as ds:
      wkt = ds.GetProjectionRef()

    self.assertEqual(wkt, _WKT_87TEST)

  def test_hfa_pe_write(self) -> None:
    filepath = self.get_test_file_path('87test.img')
    self.CheckOpen(filepath)
    self.CheckCreateCopy(vsimem=True, remove_result=False)
    self.assertIsNotNone(self.dst)
    self.assertEqual(self.dst.GetProjectionRef(), _WKT_87TEST)
    self.dst = None
    self.src = None
    if self.dst_file:
      self.driver.Delete(self.dst_file)

  def test_hfa_metadata_1(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_metadata_1') as vsimem_dir:
      filepath = f'{vsimem_dir}/md_1.img'
      with self.driver.Create(filepath, 100, 150, 1, gdal.GDT_Byte) as dst:
        md_val = '0123456789' * 60
        dst.GetRasterBand(1).SetMetadata({'test': md_val})

      with gdal.Open(filepath) as dst:
        md = dst.GetRasterBand(1).GetMetadata()
        self.assertEqual(md_val, md['test'])
        self.assertEqual(dst.FlushCache(), gdal.CE_None)

      # Verify writing metadata multiple times does not result in duplicate
      # nodes.
      with gdal.Open(filepath, gdal.GA_Update) as dst:
        md = dst.GetRasterBand(1).GetMetadata()
        md['test'] = '0123456789'
        md['xxx'] = '123'
        dst.GetRasterBand(1).SetMetadata(md)

      with gdal.Open(filepath) as dst:
        md = dst.GetRasterBand(1).GetMetadata()
        self.assertIn('xxx', md)
        self.assertEqual(md['xxx'], '123')
        self.assertEqual(md['test'], '0123456789')

      self.driver.Delete(filepath)

  def test_hfa_grow_rrdlist(self) -> None:
    with open(self.get_test_file_path('bug_1109.img'), 'rb') as f:
      data = f.read()

    with gcore_util.VsimemCleanupDir('/vsimem/hfa_grow_rrdlist') as vsimem_dir:
      filepath = f'{vsimem_dir}/bug_1109.img'
      gdal.FileFromMemBuffer(filepath, data)

      with gdal.Open(filepath, gdal.GA_Update) as ds:
        result = ds.BuildOverviews(overviewlist=[4, 8])
        self.assertEqual(result, 0)

      with gdal.Open(filepath) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 3)

      self.driver.Delete(filepath)

  def test_hfa_clean_ige(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='hfa_clean_ige') as tmpdir:
      img_path = os.path.join(tmpdir, 'igetest.img')
      ige_path = os.path.join(tmpdir, 'igetest.ige')
      with gdal.Open(gdrivers_util.GetTestFilePath('gtiff/byte.tif')) as src_ds:
        self.driver.CreateCopy(img_path, src_ds, options=['USE_SPILL=YES'])
        self.assertTrue(os.path.exists(ige_path))

        with gdal.Open(img_path) as ds:
          filelist = ds.GetFileList()
        self.assertTrue(
            any(item.endswith('igetest.ige') for item in filelist),
            f'no igetest.ige in {filelist}',
        )

        # Create a file without a spill file, and verify old .ige is cleaned up.
        self.driver.CreateCopy(img_path, src_ds)

      self.assertFalse(os.path.exists(ige_path))
      self.driver.Delete(img_path)

  def test_hfa_corrupt_aux(self) -> None:
    with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
      gdal.ErrorReset()
      with gdal.Open(self.get_test_file_path('F0116231.aux')) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.RasterXSize, 1104)
        self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
        self.assertIn('Corrupt (looping)', gdal.GetLastErrorMsg())

  def test_hfa_mapinformation_units(self) -> None:
    with gdal.quiet_errors():
      self.CheckOpen(self.get_test_file_path('fg118-91.aux'))
    expected_wkt = (
        'PROJCS["NAD_1983_StatePlane_Virginia_North_FIPS_4501_Feet",'
        'GEOGCS["GCS_North_American_1983",DATUM["North_American_Datum_1983",'
        'SPHEROID["GRS_1980",6378137,298.257222101]],PRIMEM["Greenwich",0],'
        'UNIT["Degree",0.0174532925199432955],AUTHORITY["EPSG","4269"]],'
        'PROJECTION["Lambert_Conformal_Conic_2SP"],'
        'PARAMETER["False_Easting",11482916.66666666],'
        'PARAMETER["False_Northing",6561666.666666666],'
        'PARAMETER["Central_Meridian",-78.5],'
        'PARAMETER["Standard_Parallel_1",38.03333333333333],'
        'PARAMETER["Standard_Parallel_2",39.2],'
        'PARAMETER["Latitude_Of_Origin",37.66666666666666],'
        'UNIT["Foot_US",0.304800609601219241]]'
    )
    self.CheckProjection(expected_wkt)

  def test_hfa_nodata_write(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_nodata_write') as vsimem_dir:
      nodata_img = f'{vsimem_dir}/nodata.img'
      with self.driver.Create(nodata_img, 7, 7, 1, gdal.GDT_Byte) as ds:
        raw_data = struct.pack('7h', 1, 2, 1, 4, 1, 2, 1)
        for line in range(7):
          ds.WriteRaster(0, line, 7, 1, raw_data, buf_type=gdal.GDT_Int16)
        ds.GetRasterBand(1).SetNoDataValue(1)

      with gdal.Open(nodata_img) as ds:
        band = ds.GetRasterBand(1)
        self.assertEqual(band.GetNoDataValue(), 1)
        stats = band.GetStatistics(False, True)

      self.driver.Delete(nodata_img)

    self.assertIterAlmostEqual(
        stats, (2.0, 4.0, 2.6666666666667, 0.94280904158206), delta=0.0001
    )

  def test_hfa_rotated_read(self) -> None:
    self.CheckOpen(self.get_test_file_path('fg118-91.aux'))
    check_gt = (
        11856857.07898215,
        0.895867662235625,
        0.02684252936279331,
        7041861.472946444,
        0.01962103617166367,
        -0.9007880319529181,
    )
    self.CheckGeoTransform(check_gt, gt_delta=1e-6)

  def test_hfa_rotated_write(self) -> None:
    check_gt = (
        11856857.07898215,
        0.895867662235625,
        0.02684252936279331,
        7041861.472946444,
        0.01962103617166367,
        -0.9007880319529181,
    )
    expected_wkt = (
        'PROJCS["NAD83_Virginia_North",'
        'GEOGCS["GCS_North_American_1983",DATUM["North_American_Datum_1983",'
        'SPHEROID["GRS_1980",6378137,298.257222101]],PRIMEM["Greenwich",0],'
        'UNIT["Degree",0.017453292519943295]],'
        'PROJECTION["Lambert_Conformal_Conic_2SP"],'
        'PARAMETER["standard_parallel_1",39.2],'
        'PARAMETER["standard_parallel_2",38.03333333333333],'
        'PARAMETER["latitude_of_origin",37.66666666666666],'
        'PARAMETER["central_meridian",-78.5],'
        'PARAMETER["false_easting",11482916.66666667],'
        'PARAMETER["false_northing",6561666.666666667],'
        'PARAMETER["scale_factor",1.0],UNIT["Foot_US",0.30480060960121924]]'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_rotated_write') as vsimem_dir:
      rot_img = f'{vsimem_dir}/rot.img'
      with self.driver.Create(rot_img, 100, 150, 1, gdal.GDT_Byte) as ds:
        ds.SetGeoTransform(check_gt)
        ds.SetProjection(expected_wkt)

      self.CheckOpen(rot_img)
      self.CheckGeoTransform(check_gt)
      self.CheckProjection(expected_wkt)
      self.src = None
      self.driver.Delete(rot_img)

  def test_hfa_vsimem(self) -> None:
    self.CheckOpen(
        gdrivers_util.GetTestFilePath('gtiff/byte.tif'),
        check_driver=gdrivers_util.GTIFF_DRIVER,
    )
    self.CheckBand(1, 4672, gdal.GDT_Byte)
    self.CheckCreateCopy(vsimem=True, remove_result=True)

  def test_hfa_proname(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_proname') as vsimem_dir:
      proname_img = f'{vsimem_dir}/proname.img'
      with gdal.config_option('GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE', 'ALL'):
        with gdal.Open(self.get_test_file_path('stateplane.vrt')) as src_ds:
          self.driver.CreateCopy(proname_img, src_ds)

      gdal.Unlink(f'{proname_img}.aux.xml')

      with gdal.Open(proname_img) as ds:
        srs = ds.GetProjectionRef()

      self.driver.Delete(proname_img)

    self.assertStartsWith(srs, 'PROJCS["NAD83 / Ohio South (ftUS)",')

  def test_hfa_read_empty_compressed(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_empty_comp') as vsimem_dir:
      filepath = f'{vsimem_dir}/emptycompressed.img'
      with self.driver.Create(filepath, 64, 64, 1, options=['COMPRESSED=YES']):
        pass

      with gdal.Open(filepath) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)

      self.driver.Delete(filepath)

  def test_hfa_unique_values_color_table(self) -> None:
    with gdal.Open(self.get_test_file_path('i8u_c_i.img')) as ds:
      ct = ds.GetRasterBand(1).GetRasterColorTable()
      self.assertIsNotNone(ct)
      self.assertEqual(ct.GetCount(), 256)
      self.assertEqual(ct.GetColorEntry(253), (0, 0, 0, 0))
      self.assertEqual(ct.GetColorEntry(254), (255, 255, 170, 255))
      self.assertEqual(ct.GetColorEntry(255), (255, 255, 255, 255))

  def test_hfa_unique_values_hist(self) -> None:
    expected = (
        '12603|1|0|0|45|1|0|0|0|0|656|177|0|0|5026|1062|0|0|2|0|0|0|0'
        '|0|0|0|0|0|0|0|0|0|75|1|0|0|207|158|0|0|8|34|0|0|0|0|538|57|0|10|214|20|'
        '0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|1|31|0|0|9|625|67|0|0|118|738|117|3004|1'
        '499|491|187|1272|513|1|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|16|3|0|0|283|'
        '123|5|1931|835|357|332|944|451|80|40|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0'
        '|0|0|0|12|5|0|0|535|1029|118|0|33|246|342|0|0|10|8|0|0|0|0|0|0|0|0|0|0|0'
        '|0|0|0|0|0|0|0|0|0|0|169|439|0|0|6|990|329|0|0|120|295|0|0|0|0|0|0|0|0|0'
        '|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|164|42|0|0|570|966|0|0|18|152|0|0|0|0'
        '|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|45|106|0|0|16|16517|'
    )
    with gdal.Open(self.get_test_file_path('i8u_c_i.img')) as ds:
      band = ds.GetRasterBand(1)
      md = band.GetMetadata()
      self.assertEqual(md['STATISTICS_HISTOBINVALUES'], expected)
      self.assertEqual(md['STATISTICS_HISTOMIN'], '0')
      self.assertEqual(md['STATISTICS_HISTOMAX'], '255')

      rat = band.GetDefaultRAT()
      self.assertIsNotNone(rat)
      self.assertEqual(rat.GetColumnCount(), 6)
      self.assertEqual(rat.GetTypeOfCol(0), gdal.GFT_Real)
      self.assertEqual(rat.GetUsageOfCol(0), gdal.GFU_MinMax)
      self.assertEqual(rat.GetValueAsInt(2, 0), 4)

  def test_hfa_xforms_3rd(self) -> None:
    check_list = [
        ('XFORM_STEPS', 2),
        ('XFORM0_POLYCOEFMTX[3]', -0.151286406053458),
        ('XFORM0_POLYCOEFVECTOR[1]', 401692.559078924),
        ('XFORM1_ORDER', 3),
        ('XFORM1_FWD_POLYCOEFMTX[0]', -0.560405515080768),
        ('XFORM1_FWD_POLYCOEFMTX[17]', -1.01593898110617e-08),
        ('XFORM1_REV_POLYCOEFMTX[17]', 4.01319402177037e-09),
        ('XFORM1_REV_POLYCOEFVECTOR[0]', 2605.41812438735),
    ]
    with gdal.Open(self.get_test_file_path('42BW_420730_VT2.aux')) as ds:
      xform_md = ds.GetMetadata('XFORMS')
      for key, expected in check_list:
        self.assertIn(key, xform_md)
        value = float(xform_md[key])
        self.assertAlmostEqual(
            value, expected, delta=abs(value / 100000.0), msg=key
        )

      gcps = ds.GetGCPs()
      self.assertEqual(gcps[0].GCPPixel, 0.5)
      self.assertEqual(gcps[0].GCPLine, 0.5)
      self.assertAlmostEqual(gcps[0].GCPX, 1667635.007, delta=0.001)
      self.assertAlmostEqual(gcps[0].GCPY, 2620003.171, delta=0.001)

      self.assertAlmostEqual(gcps[14].GCPPixel, 1769.7, delta=0.1)
      self.assertAlmostEqual(gcps[14].GCPLine, 2124.9, delta=0.1)
      self.assertAlmostEqual(gcps[14].GCPX, 1665221.064, delta=0.001)
      self.assertAlmostEqual(gcps[14].GCPY, 2632414.379, delta=0.001)

  def test_hfa_delete_colortable(self) -> None:
    with open(self.get_test_file_path('i8u_c_i.img'), 'rb') as f:
      data = f.read()

    with gcore_util.VsimemCleanupDir('/vsimem/hfa_del_ct') as vsimem_dir:
      filepath = f'{vsimem_dir}/i8u.img'
      gdal.FileFromMemBuffer(filepath, data)

      with gdal.Open(filepath, gdal.GA_Update) as ds:
        self.assertEqual(ds.GetRasterBand(1).SetColorTable(None), gdal.CE_None)

      with gdal.Open(filepath) as ds:
        self.assertIsNone(ds.GetRasterBand(1).GetColorTable())

      self.driver.Delete(filepath)

  def test_hfa_delete_colortable2(self) -> None:
    # Upstream uses 8bit_pal.bmp as a paletted source image for CreateCopy;
    # FRMT_bmp is not compiled in Google3, so use an in-memory paletted dataset.
    mem_driver = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    ct = gdal.ColorTable()
    ct.CreateColorRamp(0, (0, 0, 0), 255, (255, 255, 255))

    with gcore_util.VsimemCleanupDir('/vsimem/hfa_del_ct2') as vsimem_dir:
      filepath = f'{vsimem_dir}/hfa_delete_colortable2.img'
      with mem_driver.Create('', 20, 20, 1, gdal.GDT_Byte) as src_ds:
        src_ds.GetRasterBand(1).SetColorTable(ct)
        self.driver.CreateCopy(filepath, src_ds)

      with gdal.Open(filepath) as ds:
        self.assertIsNotNone(ds.GetRasterBand(1).GetColorTable())

      with gdal.Open(filepath, gdal.GA_Update) as ds:
        self.assertEqual(ds.GetRasterBand(1).SetColorTable(None), gdal.CE_None)

      with gdal.Open(filepath) as ds:
        self.assertIsNone(ds.GetRasterBand(1).GetColorTable())

      self.driver.Delete(filepath)

  def test_hfa_excluded_values(self) -> None:
    with gdal.Open(self.get_test_file_path('dem10.img')) as ds:
      md = ds.GetRasterBand(1).GetMetadata()

    self.assertEqual(md['STATISTICS_EXCLUDEDVALUES'], '0,8,9')

  def test_hfa_ov_nodata(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_ov_nodata') as vsimem_dir:
      filepath = f'{vsimem_dir}/ov_nodata.img'
      with gdal.Open(
          gdrivers_util.GetTestFilePath('aaigrid/nodata_int.asc')
      ) as src_ds:
        with self.driver.CreateCopy(filepath, src_ds) as wrk_ds:
          wrk_ds.BuildOverviews(overviewlist=[2])

      with gdal.Open(filepath) as wrk2_ds:
        ovb = wrk2_ds.GetRasterBand(1).GetOverview(0)
        self.assertEqual(ovb.GetNoDataValue(), -99999)
        self.assertEqual(ovb.GetMaskFlags(), gdal.GMF_NODATA)

      # Confirm that an .ovr file was *not* produced.
      with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
        self.assertIsNone(gdal.Open(f'{filepath}.ovr'))

      self.driver.Delete(filepath)

  def test_hfa_read_bit2grayscale(self) -> None:
    with gdal.Open(self.get_test_file_path('small1bit.img')) as ds:
      ov = ds.GetRasterBand(1).GetOverview(0)
      self.assertEqual(ov.Checksum(), 4247)
      ds_md = ds.GetMetadata()
      self.assertEqual(ds_md['PyramidResamplingType'], 'AVERAGE_BIT2GRAYSCALE')

  def test_hfa_write_bit2grayscale(self) -> None:
    with gcore_util.TestTemporaryDirectory(
        prefix='hfa_write_bit2grayscale'
    ) as tmpdir:
      img_path = os.path.join(tmpdir, 'small1bit.img')
      rrd_path = os.path.join(tmpdir, 'small1bit.rrd')
      shutil.copyfile(self.get_test_file_path('small1bit.img'), img_path)
      shutil.copyfile(self.get_test_file_path('small1bit.rrd'), rrd_path)

      with gdal.config_options({'USE_RRD': 'YES', 'HFA_USE_RRD': 'YES'}):
        with gdal.Open(img_path, gdal.GA_Update) as ds:
          ds.BuildOverviews(
              resampling='average_bit2grayscale', overviewlist=[2]
          )
          ov = ds.GetRasterBand(1).GetOverview(1)
          self.assertEqual(ov.Checksum(), 57325)

        self.driver.Delete(img_path)

      self.assertFalse(os.path.exists(rrd_path))

  def test_hfa_camera_md(self) -> None:
    sparse_path = '/vsisparse/' + self.get_test_file_path('251_sparse.xml')
    with gdal.Open(sparse_path) as ds:
      md = ds.GetMetadata('CAMERA_MODEL')

    check_list = [
        ('direction', 'EMOD_FORWARD'),
        ('forSrcAffine[0]', '0.025004093931786'),
        ('invDstAffine[0]', '1'),
        ('coeffs[1]', '-0.008'),
        ('elevationType', 'EPRJ_ELEVATION_TYPE_HEIGHT'),
    ]
    for key, expected in check_list:
      self.assertIn(key, md)
      self.assertEqual(md[key], expected)

    exp_wkt = (
        'PROJCS["UTM Zone 17, Northern Hemisphere",GEOGCS["NAD27",'
        'DATUM["North_American_Datum_1927",SPHEROID["Clarke 1866",'
        '6378206.4,294.978698213898,AUTHORITY["EPSG","7008"]],'
        'TOWGS84[-10,158,187,0,0,0,0],AUTHORITY["EPSG","6267"]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9108"]],'
        'AUTHORITY["EPSG","4267"]],PROJECTION["Transverse_Mercator"],'
        'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",-81],'
        'PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],'
        'PARAMETER["false_northing",0],UNIT["Meter",1],'
        'AUTHORITY["EPSG","26717"]]'
    )
    srs = osr.SpatialReference()
    srs.SetFromUserInput(md['outputProjection'])
    exp_srs = osr.SpatialReference()
    exp_srs.SetFromUserInput(exp_wkt)
    self.assertIsSame(srs, exp_srs)

  def test_hfa_read_tmso_projection(self) -> None:
    exp_wkt = (
        'PROJCS["Transverse Mercator (South Orientated)",GEOGCS["Cape-1",'
        'DATUM["Cape-1",SPHEROID["Clarke 1880 Arc",6378249.145,'
        '293.4663077168331],TOWGS84[-136,-108,-292,0,0,0,0]],'
        'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],'
        'PROJECTION["Transverse_Mercator_South_Orientated"],'
        'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",21],'
        'PARAMETER["scale_factor",1],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],UNIT["meters",1]]'
    )
    self.CheckOpen(self.get_test_file_path('22281.aux'))
    self.CheckProjection(exp_wkt)

  def test_hfa_write_tmso_projection(self) -> None:
    wkt = (
        'PROJCS["Hartebeesthoek94 / Lo15",GEOGCS["Hartebeesthoek94",'
        'DATUM["Hartebeesthoek94",SPHEROID["WGS 84",6378137,298.257223563,'
        'AUTHORITY["EPSG","7030"]],TOWGS84[0,0,0,0,0,0,0],'
        'AUTHORITY["EPSG","6148"]],PRIMEM["Greenwich",0,'
        'AUTHORITY["EPSG","8901"]],UNIT["degree",0.01745329251994328,'
        'AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4148"]],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],'
        'PROJECTION["Transverse_Mercator_South_Orientated"],'
        'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",15],'
        'PARAMETER["scale_factor",1],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],AUTHORITY["EPSG","2046"],'
        'AXIS["Y",WEST],AXIS["X",SOUTH]]'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_write_tmso') as vsimem_dir:
      filepath = f'{vsimem_dir}/tmso.img'
      with self.driver.Create(filepath, 1, 1) as out_ds:
        out_ds.SetGeoTransform((0, 1, 0, 0, 0, 1))
        out_ds.SetProjection(wkt)

      self.CheckOpen(filepath)
      self.CheckProjection(wkt)
      self.src = None
      self.driver.Delete(filepath)

  def test_hfa_read_homva_projection(self) -> None:
    # Upstream opens 3376.tif, relying on PAM TryLoadAux to load 3376.aux;
    # PAM_ENABLED is disabled in Google3, so open 3376.aux directly with HFA.
    exp_wkt = (
        'PROJCS["Hotine Oblique Mercator (Variant A)",GEOGCS["GDM 2000",'
        'DATUM["Geodetic_Datum_of_Malaysia_2000",SPHEROID["GRS 1980",'
        '6378137,298.257222096042],TOWGS84[0,0,0,0,0,0,0]],'
        'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,'
        'AUTHORITY["EPSG","9122"]]],PROJECTION["Hotine_Oblique_Mercator"],'
        'PARAMETER["latitude_of_center",4],'
        'PARAMETER["longitude_of_center",115],'
        'PARAMETER["azimuth",53.31580995],'
        'PARAMETER["rectified_grid_angle",53.1301023611111],'
        'PARAMETER["scale_factor",0.99984],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],UNIT["meters",1],'
        'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
    )
    with gdal.Open(self.get_test_file_path('3376.aux')) as ds:
      srs_got = osr.SpatialReference(wkt=ds.GetProjectionRef())

    srs_exp = osr.SpatialReference()
    srs_exp.SetFromUserInput(exp_wkt)
    srs_exp_alt = osr.SpatialReference()
    srs_exp_alt.SetFromUserInput(
        exp_wkt.replace('Geodetic_Datum_of_Malaysia_2000', 'GDM 2000')
    )
    self.assertTrue(
        srs_got.IsSame(srs_exp) or srs_got.IsSame(srs_exp_alt),
        srs_got.ExportToWkt(),
    )

  def test_hfa_write_homva_projection(self) -> None:
    input_wkt = (
        'PROJCS["Hotine Oblique Mercator (Variant A)",GEOGCS["GDM 2000",'
        'DATUM["GDM_2000",SPHEROID["GRS 1980",6378137,298.2572220960422],'
        'TOWGS84[0,0,0,0,0,0,0]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]],'
        'PROJECTION["Hotine_Oblique_Mercator"],'
        'PARAMETER["latitude_of_center",4],'
        'PARAMETER["longitude_of_center",115],'
        'PARAMETER["azimuth",53.31580995],'
        'PARAMETER["rectified_grid_angle",53.13010236111111],'
        'PARAMETER["scale_factor",0.99984],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],UNIT["meters",1]]'
    )
    exp_wkt = (
        'PROJCS["Hotine Oblique Mercator (Variant A)",GEOGCS["GDM_2000",'
        'DATUM["GDM_2000",SPHEROID["GRS 1980",6378137,298.257222096042],'
        'TOWGS84[0,0,0,0,0,0,0]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],'
        'PROJECTION["Hotine_Oblique_Mercator"],'
        'PARAMETER["latitude_of_center",4],'
        'PARAMETER["longitude_of_center",115],'
        'PARAMETER["azimuth",53.31580995],'
        'PARAMETER["rectified_grid_angle",53.1301023611111],'
        'PARAMETER["scale_factor",0.99984],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],UNIT["meters",1],'
        'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_write_homva') as vsimem_dir:
      filepath = f'{vsimem_dir}/homva.img'
      with self.driver.Create(filepath, 1, 1) as out_ds:
        out_ds.SetGeoTransform((0, 1, 0, 0, 0, 1))
        out_ds.SetProjection(input_wkt)

      self.CheckOpen(filepath)
      self.CheckProjection(exp_wkt)
      self.src = None
      self.driver.Delete(filepath)

  def test_hfa_rde_overviews(self) -> None:
    with gdal.Open(self.get_test_file_path('spill.img')) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 1631)
      self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 340)
      filelist = [os.path.basename(x) for x in ds.GetFileList()]

    self.assertEqual(
        filelist, ['spill.img', 'spill.ige', 'spill.rrd', 'spill.rde']
    )

  def test_hfa_copyfiles(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='hfa_copyfiles') as tmpdir:
      after_copy = os.path.join(tmpdir, 'newnamexxx_after_copy.img')
      renamed = os.path.join(tmpdir, 'newnamexxx.img')
      self.driver.CopyFiles(after_copy, self.get_test_file_path('spill.img'))
      self.driver.Rename(renamed, after_copy)

      with gdal.Open(renamed) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 340)
        filelist = ds.GetFileList()

      self.assertEqual(
          filelist,
          [
              os.path.join(tmpdir, 'newnamexxx.img'),
              os.path.join(tmpdir, 'newnamexxx.ige'),
              os.path.join(tmpdir, 'newnamexxx.rrd'),
              os.path.join(tmpdir, 'newnamexxx.rde'),
          ],
      )

      # Check that internal filenames were updated in .img and .rrd.
      with open(renamed, 'rb') as f:
        img_bytes = f.read()
      self.assertIn(b'newnamexxx.rrd', img_bytes)
      self.assertIn(b'newnamexxx.ige', img_bytes)

      with open(os.path.join(tmpdir, 'newnamexxx.rrd'), 'rb') as f:
        rrd_bytes = f.read()
      self.assertIn(b'newnamexxx.img', rrd_bytes)
      self.assertIn(b'newnamexxx.rde', rrd_bytes)

      self.driver.Delete(renamed)

  def test_hfa_write_rat(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_write_rat') as vsimem_dir:
      filepath = f'{vsimem_dir}/write_rat.img'
      with gdal.Open(self.get_test_file_path('i8u_c_i.img')) as src_ds:
        rat = src_ds.GetRasterBand(1).GetDefaultRAT()
        with self.driver.Create(filepath, 100, 100, 1, gdal.GDT_Byte) as dst_ds:
          dst_ds.GetRasterBand(1).SetDefaultRAT(rat)

      with gdal.Open(filepath) as ds:
        rat = ds.GetRasterBand(1).GetDefaultRAT()
        self.assertIsNotNone(rat)
        self.assertEqual(rat.GetColumnCount(), 6)
        self.assertEqual(rat.GetTypeOfCol(0), gdal.GFT_Real)
        self.assertEqual(rat.GetUsageOfCol(0), gdal.GFU_Generic)
        self.assertEqual(rat.GetValueAsInt(2, 0), 4)
        self.assertEqual(rat.GetValueAsInt(4, 5), 656)

      self.driver.Delete(filepath)

  def test_hfa_createcopy_statistics(self) -> None:
    with open(gdrivers_util.GetTestFilePath('gtiff/byte.tif'), 'rb') as f:
      tif_data = f.read()

    with gcore_util.VsimemCleanupDir('/vsimem/hfa_stats') as vsimem_dir:
      src_tif = f'{vsimem_dir}/byte.tif'
      out_img = f'{vsimem_dir}/byte.img'
      gdal.FileFromMemBuffer(src_tif, tif_data)

      with gdal.Open(src_tif) as ds_src:
        self.driver.CreateCopy(out_img, ds_src, options=['STATISTICS=YES'])

      gdal.Unlink(f'{out_img}.aux.xml')

      with gdal.Open(out_img) as ds:
        md = ds.GetRasterBand(1).GetMetadata()

      self.driver.Delete(out_img)

    self.assertEqual(md['STATISTICS_MINIMUM'], '74')

  @parameterized.parameters(
      ('erdas_cm.img', 'cm'),
      ('erdas_feet.img', 'feet'),
      ('erdas_m.img', 'meters'),
  )
  def test_hfa_read_elevation_units(
      self, filename: str, expected_units: str
  ) -> None:
    with gdal.Open(self.get_test_file_path(filename)) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetUnitType(), expected_units)

  def test_hfa_read_nan_nodata(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_nan_nodata') as vsimem_dir:
      filepath = f'{vsimem_dir}/test.img'
      with self.driver.Create(filepath, 1, 1, 1, gdal.GDT_Float32) as ds:
        ds.GetRasterBand(1).SetNoDataValue(math.nan)

      with gdal.ExceptionMgr(useExceptions=False):
        gdal.ErrorReset()
        with gdal.Open(filepath) as ds:
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          self.assertTrue(math.isnan(ds.GetRasterBand(1).GetNoDataValue()))

      self.driver.Delete(filepath)

  def test_hfa_rat_grow_string(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_rat_grow_str') as vsimem_dir:
      filepath = f'{vsimem_dir}/test.img'
      with self.driver.Create(filepath, 1, 1, 1, gdal.GDT_Byte) as ds:
        rat = gdal.RasterAttributeTable()
        rat.CreateColumn('str', gdal.GFT_String, gdal.GFU_Generic)
        rat.SetValueAsString(0, 0, 'x' * 5)
        rat.SetValueAsString(1, 0, 'y' * 5)
        ds.GetRasterBand(1).SetDefaultRAT(rat)

      with gdal.Open(filepath, gdal.GA_Update) as ds:
        rat = ds.GetRasterBand(1).GetDefaultRAT()
        rat.SetValueAsString(0, 0, 'x' * 50)

      with gdal.Open(filepath, gdal.GA_Update) as ds:
        rat = ds.GetRasterBand(1).GetDefaultRAT()
        self.assertEqual(rat.GetValueAsString(0, 0), 'x' * 50)
        self.assertEqual(rat.GetValueAsString(1, 0), 'y' * 5)

      self.driver.Delete(filepath)

  def test_hfa_rat_new_types(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/hfa_rat_new_types') as vsimem_dir:
      filepath = f'{vsimem_dir}/test.img'
      with self.driver.Create(filepath, 1, 1, 1, gdal.GDT_Byte) as ds:
        rat = gdal.RasterAttributeTable()
        rat.CreateColumn('bool', gdal.GFT_Boolean, gdal.GFU_Generic)
        rat.CreateColumn('datetime', gdal.GFT_DateTime, gdal.GFU_Generic)
        rat.CreateColumn('wkbgeometry', gdal.GFT_WKBGeometry, gdal.GFU_Generic)
        rat.SetValueAsBoolean(0, 0, True)
        rat.SetValueAsString(0, 1, '2025-10-11T12:34:56.500+01:30')
        rat.SetValueAsString(0, 2, 'POINT (1.0 2)')
        ds.GetRasterBand(1).SetDefaultRAT(rat)

      with gdal.Open(filepath, gdal.GA_Update) as ds:
        # Google3 patches out 'tableType' in GDALRasterAttributeTable::
        # SerializeJSON() (patches/gcore-gdal_rat-cpp.patch).
        rat_info = gdal.Info(ds, format='json')['bands'][0]['rat']
        self.assertEqual(
            rat_info,
            {
                'fieldDefn': [
                    {'index': 0, 'name': 'bool', 'type': 0, 'usage': 0},
                    {'index': 1, 'name': 'datetime', 'type': 2, 'usage': 0},
                    {'index': 2, 'name': 'wkbgeometry', 'type': 2, 'usage': 0},
                ],
                'row': [{
                    'f': [
                        1,
                        '2025-10-11T12:34:56.500+01:30',
                        'POINT (1 2)',
                    ],
                    'index': 0,
                }],
            },
        )

        rat = ds.GetRasterBand(1).GetDefaultRAT()
        rat.SetValueAsBoolean(0, 0, True)
        self.assertTrue(rat.GetValueAsBoolean(0, 0))

        dt = datetime.datetime(
            2025,
            12,
            31,
            23,
            58,
            59,
            500000,
            tzinfo=datetime.timezone(datetime.timedelta(seconds=4500)),
        )
        rat.SetValueAsDateTime(0, 1, dt)
        self.assertEqual(
            rat.GetValueAsString(0, 1), '2025-12-31T23:58:59.500+01:15'
        )
        self.assertEqual(rat.GetValueAsDateTime(0, 1), dt)

        geom = ogr.Geometry(ogr.wkbPoint)
        geom.SetPoint_2D(0, 1, 2)
        wkb = geom.ExportToWkb()
        rat.SetValueAsWKBGeometry(0, 2, wkb)
        self.assertEqual(rat.GetValueAsWKBGeometry(0, 2), wkb)

      self.driver.Delete(filepath)


if __name__ == '__main__':
  googletest.main()
