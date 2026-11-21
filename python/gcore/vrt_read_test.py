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
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT

"""Tests VRT raster reading and vrt:// protocol support.

Rewrite of:

https://github.com/OSGeo/gdal/blob/c5a57bae66dcd36740ab9cfdf5459185824cb4af/autotest/gcore/vrt_read.py

Pinned to upstream commit c5a57bae (GDAL 3.12.0beta1, matching METADATA). Five
post-c5a57bae master-only tests (invalid color table entry, vrt:// block=,
recursion false-positive 2, and MT non-integer nearest reads) target C++ fixes
not yet in Google3 and are omitted. NetCDF/Zarr replace HDF5 fwhm.h5 where
cross-package gdrivers/testdata access is avoided.
"""

import array
import contextlib
import ctypes
import os
import pathlib
import struct
import subprocess
import sys

import numpy as np
from osgeo import gdal
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

_DATA_TYPE_TO_STRUCT_CHAR = {
    gdal.GDT_Byte: 'B',
    gdal.GDT_Int8: 'b',
    gdal.GDT_UInt16: 'H',
    gdal.GDT_Int16: 'h',
    gdal.GDT_UInt32: 'I',
    gdal.GDT_Int32: 'i',
    gdal.GDT_UInt64: 'Q',
    gdal.GDT_Int64: 'q',
    gdal.GDT_Float32: 'f',
    gdal.GDT_Float64: 'd',
}


_SMALL_POOL_CHILD_FLAG = '--vrt-small-pool-child'


def _is_tsan() -> bool:
  """Returns whether this process was built with ThreadSanitizer."""
  try:
    return hasattr(ctypes.CDLL(None), '__tsan_init')
  except OSError:
    return False


def _verify_two_level_vrt_small_pool(tmp_dir: pathlib.Path) -> None:
  """Verifies two-level VRT reading under GDAL_MAX_DATASET_POOL_SIZE=3."""
  if gdal.GetConfigOption('GDAL_MAX_DATASET_POOL_SIZE') != '3':
    raise AssertionError('Expected GDAL_MAX_DATASET_POOL_SIZE=3 in child env')
  if gdal.GetConfigOption('VRT_SHARED_SOURCE') != '0':
    raise AssertionError('Expected VRT_SHARED_SOURCE=0 in child env')

  with gdal.ExceptionMgr():
    with gdal.OpenEx(
        str(tmp_dir / 'byte.tif'), gdal.OF_RASTER | gdal.OF_READONLY
    ) as src_ds:
      expected_arr = src_ds.GetRasterBand(1).ReadAsArray()
    with gdal.OpenEx(
        str(tmp_dir / 'byte2.vrt'), gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      band = ds.GetRasterBand(1)
      if band.Checksum() != 4672:
        raise AssertionError(f'Expected checksum 4672, got {band.Checksum()}')
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)

    # Confirm the singleton pool size of 3 is actively enforced by opening a
    # 4-level cascade (requires >3 simultaneous open datasets in the pool).
    try:
      with gdal.quiet_errors():
        with gdal.OpenEx(
            str(tmp_dir / 'lvl4.vrt'), gdal.OF_RASTER | gdal.OF_READONLY
        ) as lvl4_ds:
          lvl4_ds.GetRasterBand(1).ReadAsArray()
    except RuntimeError as exc:
      if 'GDAL_MAX_DATASET_POOL_SIZE' not in str(exc):
        raise AssertionError(
            f'Expected GDAL_MAX_DATASET_POOL_SIZE error, got: {exc}'
        ) from exc
    else:
      raise AssertionError(
          'Expected 4-level VRT cascade to exceed GDAL_MAX_DATASET_POOL_SIZE=3'
      )

  print('SMALL_POOL_OK checksum=4672 pool_limit_enforced=1', flush=True)


def _count_open_fds_for_path(target_path: pathlib.Path) -> int | None:
  """Counts open /proc/self/fd entries pointing to target_path.

  Args:
    target_path: Real filesystem path to match against open file descriptors.

  Returns:
    Count of matching descriptors, or None if /proc/<pid>/fd is unavailable.
  """
  fd_dir = pathlib.Path(f'/proc/{os.getpid()}/fd')
  if not fd_dir.exists():
    return None
  resolved_target = os.path.realpath(target_path)
  count = 0
  for entry in fd_dir.iterdir():
    try:
      link_target = os.readlink(entry)
    except OSError:
      continue
    if os.path.realpath(link_target) == resolved_target:
      count += 1
  return count


class VrtReadTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    self.driver = gdal.GetDriverByName(gdrivers_util.VRT_DRIVER)
    self.assertIsNotNone(self.driver)
    self.gtiff_driver = gdal.GetDriverByName(gdrivers_util.GTIFF_DRIVER)
    self.assertIsNotNone(self.gtiff_driver)

  def _make_vsi_dir(self) -> pathlib.Path:
    safe_name = self._testMethodName.replace('/', '_')
    return pathlib.Path(
        self.enter_context(
            gcore_util.VsimemCleanupDir(f'/vsimem/vrt_read_test/{safe_name}')
        )
    )

  def _vrt_threads_config(
      self, threads: str | None = None
  ) -> contextlib.AbstractContextManager[object]:
    if threads == '0':
      return gdal.config_option('VRT_NUM_THREADS', '0', thread_local=False)
    if _is_tsan():
      # TODO(schwehr): Re-enable multi-threaded TSan once upstream
      # GDALDriver::Open() and GDALRasterBlock::Touch() (b/581830013) races
      # are fixed.
      return gdal.config_option('VRT_NUM_THREADS', '1', thread_local=False)
    if threads is None:
      return contextlib.nullcontext()
    return gdal.config_option('VRT_NUM_THREADS', threads, thread_local=False)

  # Upstream: test_vrt_open
  @parameterized.named_parameters(
      dict(
          testcase_name='byte',
          filename='byte.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='int16',
          filename='int16.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_Int16,
      ),
      dict(
          testcase_name='uint16',
          filename='uint16.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_UInt16,
      ),
      dict(
          testcase_name='int32',
          filename='int32.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_Int32,
      ),
      dict(
          testcase_name='uint32',
          filename='uint32.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_UInt32,
      ),
      dict(
          testcase_name='float32',
          filename='float32.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_Float32,
      ),
      dict(
          testcase_name='float64',
          filename='float64.vrt',
          band_num=1,
          checksum=4672,
          band_type=gdal.GDT_Float64,
      ),
      dict(
          testcase_name='cint16',
          filename='cint16.vrt',
          band_num=1,
          checksum=5028,
          band_type=gdal.GDT_CInt16,
      ),
      dict(
          testcase_name='cint32',
          filename='cint32.vrt',
          band_num=1,
          checksum=5028,
          band_type=gdal.GDT_CInt32,
      ),
      dict(
          testcase_name='cfloat32',
          filename='cfloat32.vrt',
          band_num=1,
          checksum=5028,
          band_type=gdal.GDT_CFloat32,
      ),
      dict(
          testcase_name='cfloat64',
          filename='cfloat64.vrt',
          band_num=1,
          checksum=5028,
          band_type=gdal.GDT_CFloat64,
      ),
      dict(
          testcase_name='msubwinbyte',
          filename='msubwinbyte.vrt',
          band_num=2,
          checksum=2699,
          band_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='utmsmall',
          filename='utmsmall.vrt',
          band_num=1,
          checksum=50054,
          band_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_nearest_50pct',
          filename='byte_nearest_50pct.vrt',
          band_num=1,
          checksum=1192,
          band_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_averaged_50pct',
          filename='byte_averaged_50pct.vrt',
          band_num=1,
          checksum=1152,
          band_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_nearest_200pct',
          filename='byte_nearest_200pct.vrt',
          band_num=1,
          checksum=18784,
          band_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_averaged_200pct',
          filename='byte_averaged_200pct.vrt',
          band_num=1,
          checksum=18784,
          band_type=gdal.GDT_Byte,
      ),
  )
  def test_open_datatypes_and_variants(
      self,
      filename: str,
      band_num: int,
      checksum: int,
      band_type: int,
  ) -> None:
    filepath = gcore_util.GetTestFilePath(filename)
    self.assertTrue(pathlib.Path(filepath).exists())
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          ds.GetDriver().ShortName.lower(), gdrivers_util.VRT_DRIVER
      )
      band = ds.GetRasterBand(band_num)
      self.assertEqual(band.DataType, band_type)
      self.assertEqual(band.Checksum(), checksum)
      arr = band.ReadAsArray()
      self.assertEqual(arr.shape, (ds.RasterYSize, ds.RasterXSize))

  # Upstream: test_vrt_read_non_existing_source
  @parameterized.named_parameters(
      dict(testcase_name='idontexist', filename='idontexist.vrt'),
      dict(testcase_name='idontexist2', filename='idontexist2.vrt'),
  )
  def test_non_existing_source(self, filename: str) -> None:
    filepath = gcore_util.GetTestFilePath(filename)
    self.assertTrue(pathlib.Path(filepath).exists())
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError,
          r'Checksum value could not be computed due to I/O read error',
      ):
        ds.GetRasterBand(1).Checksum()
      self.assertEqual(ds.GetMetadata(), {})
      self.assertEqual(ds.GetRasterBand(1).GetMetadata(), {})
      self.assertEqual(ds.GetGCPs(), ())

  # Upstream: test_vrt_read_3
  def test_cascaded_vrt_band_init(self) -> None:
    # https://trac.osgeo.org/gdal/ticket/2867
    filepath = gcore_util.GetTestFilePath('test_mosaic.vrt')
    self.assertTrue(pathlib.Path(filepath).exists())
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      # Checksum() reads line by line; ReadRaster uses multi-line IRasterIO.
      arr = ds.GetRasterBand(1).ReadAsArray(90, 0, 20, 100)
    self.assertEqual(arr.shape, (100, 20))
    np.testing.assert_array_equal(arr[:, 9], np.full(100, 255, dtype=np.uint8))

  # Upstream: test_vrt_read_4
  def test_complex_source_complex_data_scaling(self) -> None:
    # https://trac.osgeo.org/gdal/ticket/3977
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'test.tif')
    with self.gtiff_driver.Create(tif_path, 1, 1, 1, gdal.GDT_CFloat32) as dst:
      dst.GetRasterBand(1).WriteArray(
          np.array([[1.0 + 3.0j]], dtype=np.complex64)
      )

    vrt_xml = (
        '<VRTDataset rasterXSize="1" rasterYSize="1">\n'
        '  <VRTRasterBand dataType="CFloat32" band="1">\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename relativeToVRT="0">{tif_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <ScaleOffset>3</ScaleOffset>\n'
        '      <ScaleRatio>2</ScaleRatio>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>\n'
    )

    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      src_data = ds.GetRasterBand(1).ReadAsArray()
    np.testing.assert_allclose(
        src_data, np.array([[5.0 + 9.0j]], dtype=np.complex64)
    )

  # Upstream: test_vrt_read_5
  def test_serialize_and_deserialize_band_metadata(self) -> None:
    # https://trac.osgeo.org/gdal/ticket/5326
    vsi_dir = self._make_vsi_dir()
    vrt_path = str(vsi_dir / 'vrt_read_5.vrt')
    src_filepath = gcore_util.GetTestFilePath('testserialization.asc')
    self.assertTrue(pathlib.Path(src_filepath).exists())

    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with gdal.OpenEx(src_filepath, gdal.OF_RASTER | gdal.OF_READONLY) as src:
        with self.driver.CreateCopy(vrt_path, src):
          pass

      with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        gcps = ds.GetGCPs()
        self.assertLen(gcps, 2)
        self.assertEqual(ds.GetGCPCount(), 2)
        self.assertIn('WGS 84', ds.GetGCPProjection())

        ds.SetGCPs(gcps, ds.GetGCPProjection())
        self.assertLen(ds.GetGCPs(), 2)
        self.assertEqual(ds.GetGCPCount(), 2)
        self.assertIn('WGS 84', ds.GetGCPProjection())

        band = ds.GetRasterBand(1)
        self.assertEqual(band.GetDescription(), 'MyDescription')
        self.assertEqual(band.GetUnitType(), 'MyUnit')
        self.assertEqual(band.GetOffset(), 1)
        self.assertEqual(band.GetScale(), 2)
        self.assertEqual(
            band.GetRasterColorInterpretation(), gdal.GCI_PaletteIndex
        )
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
            'STATISTICS_STDDEV': '3',
        }
        self.assertEqual(band.GetMetadata(), expected_metadata)

  # Upstream: test_vrt_read_6
  def test_min_max_updated_from_source_stats(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'vrt_read_6.tif')
    vrt_path = str(vsi_dir / 'vrt_read_6.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds) as mem_ds:
        with self.driver.CreateCopy(vrt_path, mem_ds) as vrt_ds:
          self.assertIsNone(vrt_ds.GetRasterBand(1).GetMinimum())
          self.assertIsNone(vrt_ds.GetRasterBand(1).GetMaximum())
          mem_ds.GetRasterBand(1).ComputeStatistics(False)
          self.assertEqual(vrt_ds.GetRasterBand(1).GetMinimum(), 74)
          self.assertEqual(vrt_ds.GetRasterBand(1).GetMaximum(), 255)

  # Upstream: test_vrt_read_min_max_several_sources
  def test_min_max_across_multiple_sources(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    src1 = str(tmp_dir / 'left.tif')
    src2 = str(tmp_dir / 'right.tif')
    vrt = str(tmp_dir / 'mosaic.vrt')
    byte_path = gcore_util.GetTestFilePath('byte.tif')

    with gdal.Translate(src1, byte_path, srcWin=[0, 0, 10, 20]) as ds1:
      ds1.GetRasterBand(1).ComputeStatistics(False)
    with gdal.Translate(src2, byte_path, srcWin=[10, 0, 10, 20]) as ds2:
      ds2.GetRasterBand(1).ComputeStatistics(False)
    with gdal.BuildVRT(vrt, [src1, src2]) as vrt_ds:
      self.assertEqual(vrt_ds.GetRasterBand(1).GetMinimum(), 74)
      self.assertEqual(vrt_ds.GetRasterBand(1).GetMaximum(), 255)

  # Upstream: test_vrt_read_7
  def test_self_referencing_vrt_recursion_detected(self) -> None:
    vsi_dir = self._make_vsi_dir()
    filename = str(vsi_dir / 'self_ref.vrt')
    content = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{filename}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    gdal.FileFromMemBuffer(filename, content)
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError, r'Recursion detected'
      ):
        ds.GetRasterBand(1).Checksum()

  # Upstream: test_vrt_read_8
  def test_compute_raster_min_max(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'vrt_read_8.tif')
    vrt_path = str(vsi_dir / 'vrt_read_8.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds) as mem_ds:
        with self.driver.CreateCopy(vrt_path, mem_ds) as vrt_ds:
          self.assertEqual(
              vrt_ds.GetRasterBand(1).ComputeRasterMinMax(),
              mem_ds.GetRasterBand(1).ComputeRasterMinMax(),
          )
          self.assertEqual(
              vrt_ds.GetRasterBand(1).ComputeRasterMinMax(), (74.0, 255.0)
          )

  # Upstream: test_vrt_read_9
  def test_compute_statistics(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'vrt_read_9.tif')
    vrt_path = str(vsi_dir / 'vrt_read_9.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds) as mem_ds:
        with self.driver.CreateCopy(vrt_path, mem_ds) as vrt_ds:
          vrt_stats = vrt_ds.GetRasterBand(1).ComputeStatistics(False)
          mem_stats = mem_ds.GetRasterBand(1).ComputeStatistics(False)
          for vrt_val, mem_val in zip(vrt_stats, mem_stats, strict=True):
            self.assertAlmostEqual(vrt_val, mem_val)

  # Upstream: test_vrt_read_10 (CreateCopy case)
  def test_histogram_create_copy_serializes(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'vrt_read_10.tif')
    vrt_path = str(vsi_dir / 'vrt_read_10.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds) as mem_ds:
        mem_hist = mem_ds.GetRasterBand(1).GetHistogram()
        with self.driver.CreateCopy(vrt_path, mem_ds) as vrt_ds:
          vrt_hist = vrt_ds.GetRasterBand(1).GetHistogram()

    content = gcore_util.vsi_read_file_as_string(vrt_path, 10000)
    self.assertEqual(vrt_hist, mem_hist)
    self.assertIn('<Histograms>', content)

  # Upstream: test_vrt_read_10 (single-source and two-source cases)
  @parameterized.named_parameters(
      dict(
          testcase_name='single_source_default',
          two_sources=False,
          use_default=True,
      ),
      dict(
          testcase_name='single_source_custom',
          two_sources=False,
          use_default=False,
      ),
      dict(
          testcase_name='two_sources_default',
          two_sources=True,
          use_default=True,
      ),
      dict(
          testcase_name='two_sources_custom',
          two_sources=True,
          use_default=False,
      ),
  )
  def test_histogram_serialization_sources(
      self, two_sources: bool, use_default: bool
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'src.tif')
    vrt_path = str(vsi_dir / 'hist.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds):
        pass

    if two_sources:
      second_source = (
          '    <SimpleSource>\n'
          '      <SourceFilename relativeToVRT="1">src.tif</SourceFilename>\n'
          '    </SimpleSource>\n'
      )
    else:
      second_source = ''
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="1">src.tif</SourceFilename>\n'
        '    </SimpleSource>\n'
        f'{second_source}'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    gdal.FileFromMemBuffer(vrt_path, vrt_xml)

    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      if use_default:
        hist = band.GetDefaultHistogram()
        self.assertIsNotNone(hist)
      else:
        hist = band.GetHistogram()
        self.assertLen(hist, 256)

    content = gcore_util.vsi_read_file_as_string(vrt_path, 10000)
    self.assertIn('<Histograms>', content)

  # Upstream: test_vrt_read_11, test_vrt_read_12, test_vrt_read_13
  @parameterized.named_parameters(
      dict(testcase_name='abs_symlink', chained=False, relative=False),
      dict(testcase_name='rel_symlink', chained=False, relative=True),
      dict(testcase_name='chained_rel_symlink', chained=True, relative=True),
  )
  def test_symlinked_vrt_does_not_dereference_in_google3(
      self, chained: bool, relative: bool
  ) -> None:
    # Google3 disables readlink() in VRTDataset::Open (see
    # third_party/gdal/patches/frmts-vrt-vrtdataset-cpp.patch) so CAS symlinks
    # in Forge/Borg sandboxes do not break relativeToVRT="1" lookups.
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    target_dir = tmp_dir / 'target'
    target_dir.mkdir()

    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as byte_ds:
      expected_arr = byte_ds.GetRasterBand(1).ReadAsArray()
      with self.gtiff_driver.CreateCopy(str(target_dir / 'byte.tif'), byte_ds):
        pass

    (target_dir / 'byte.vrt').write_text(
        pathlib.Path(gcore_util.GetTestFilePath('byte.vrt')).read_text(
            encoding='utf-8'
        ),
        encoding='utf-8',
    )

    def make_symlink_path(link_dir: pathlib.Path) -> pathlib.Path:
      link_dir.mkdir()
      first_link = link_dir / 'byte.vrt'
      if relative:
        os.symlink(
            os.path.relpath(target_dir / 'byte.vrt', link_dir), first_link
        )
      else:
        os.symlink(target_dir / 'byte.vrt', first_link)
      if not chained:
        return first_link
      second_link = link_dir / 'other_byte.vrt'
      os.symlink('byte.vrt', second_link)
      return second_link

    # Use distinct link directories so a cached failed-open entry in
    # GDALDatasetPool (TODO(schwehr): file bug for gcore/gdalproxypool.cpp
    # re-serving poDS=nullptr entries keyed by VRT pointer) cannot mask adding
    # byte.tif later at the same heap address.
    missing_path = make_symlink_path(tmp_dir / 'links_missing')
    with gdal.OpenEx(
        str(missing_path), gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError, r'No such file or directory'
      ):
        ds.GetRasterBand(1).ReadAsArray()

    # Placing byte.tif alongside the symlink succeeds and returns the pixels.
    present_dir = tmp_dir / 'links_present'
    present_path = make_symlink_path(present_dir)
    (present_dir / 'byte.tif').write_bytes(
        (target_dir / 'byte.tif').read_bytes()
    )
    with gdal.OpenEx(
        str(present_path), gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), expected_arr
      )

  # Upstream: test_vrt_read_14
  def test_compute_statistics_subwindow(self) -> None:
    # https://trac.osgeo.org/gdal/ticket/5468
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'vrt_read_14.tif')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      sub_arr = src_ds.GetRasterBand(1).ReadAsArray(2, 2, 4, 4)
      with self.gtiff_driver.CreateCopy(tif_path, src_ds) as copy_ds:
        copy_ds.FlushCache()

    vrt_xml = (
        '<VRTDataset rasterXSize="4" rasterYSize="4">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{tif_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="2" yOff="2" xSize="4" ySize="4" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="4" ySize="4" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      vrt_stats = vrt_ds.GetRasterBand(1).ComputeStatistics(False)

    self.assertEqual(vrt_stats[0], 115.0)
    self.assertEqual(vrt_stats[1], 173.0)
    self.assertAlmostEqual(vrt_stats[2], float(np.mean(sub_arr)))
    self.assertAlmostEqual(vrt_stats[3], float(np.std(sub_arr)))

  # Upstream: test_vrt_read_15, test_vrt_read_16
  @parameterized.named_parameters(
      dict(testcase_name='simple_source', source_tag='SimpleSource'),
      dict(testcase_name='complex_source', source_tag='ComplexSource'),
  )
  def test_rasterio_resampling_duplicate_sources(self, source_tag: str) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    source_xml = (
        f'    <{source_tag}>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="9" ySize="9" />\n'
        f'    </{source_tag}>\n'
    )
    vrt_xml = (
        '<VRTDataset rasterXSize="9" rasterYSize="9">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        f'{source_xml}{source_xml}'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.Translate(
        '', byte_path, format='MEM', width=9, height=9
    ) as expected_ds:
      expected_arr = expected_ds.GetRasterBand(1).ReadAsArray()

    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 1044)
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)

  # Upstream: test_vrt_read_17
  def test_rasterio_resampling_averaged_source(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="9" rasterYSize="9">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <AveragedSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="9" ySize="9" />\n'
        '    </AveragedSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      with mem_drv.CreateCopy('', vrt_ds) as mem_ds:
        band = mem_ds.GetRasterBand(1)
        self.assertEqual(band.Checksum(), 799)
        arr = band.ReadAsArray()
    self.assertEqual(arr.shape, (9, 9))
    self.assertEqual(int(arr[0, 0]), 119)

  # Upstream: test_vrt_read_18
  def test_vrt_in_vrt_cdata_relative_path(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'byte.tif')
    vrt_path = str(vsi_dir / 'vrtinvrt.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      expected_arr = src_ds.GetRasterBand(1).ReadAsArray()
      with self.gtiff_driver.CreateCopy(tif_path, src_ds):
        pass

    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        '      <SourceFilename><![CDATA[\n'
        '        <VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '          <VRTRasterBand dataType="Byte" band="1">\n'
        '            <SimpleSource>\n'
        '              <SourceFilename relativeToVRT="1">byte.tif'
        '</SourceFilename>\n'
        '              <SourceBand>1</SourceBand>\n'
        '              <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '              <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '              <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '            </SimpleSource>\n'
        '          </VRTRasterBand>\n'
        '        </VRTDataset>\n'
        '      ]]></SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    gdal.FileFromMemBuffer(vrt_path, vrt_xml)
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 4672)
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)

  # Upstream: test_vrt_read_19
  @parameterized.named_parameters(
      dict(testcase_name='with_source_properties', with_props=True),
      dict(testcase_name='without_source_properties', with_props=False),
  )
  def test_averaged_source_shared_zero(self, with_props: bool) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    if with_props:
      props = (
          '      <SourceProperties RasterXSize="20" RasterYSize="20"'
          ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
      )
    else:
      props = ''
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <AveragedSource>\n'
        f'      <SourceFilename relativeToVRT="0" shared="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        f'{props}'
        '    </AveragedSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(byte_path, gdal.OF_RASTER | gdal.OF_READONLY) as src_ds:
      expected_arr = src_ds.GetRasterBand(1).ReadAsArray()
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 4672)
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)

  # Upstream: test_vrt_read_20
  def test_two_level_vrt_shared_zero_small_pool(self) -> None:
    # GDALDatasetPool::singleton latches GDAL_MAX_DATASET_POOL_SIZE on first
    # use and a nested VRT leaks a pool reference (TODO(schwehr): file bug for
    # gcore/gdalproxypool.cpp refCountOfDisabledRefCount asymmetry), so isolate
    # in a child process.
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(str(tmp_dir / 'byte.tif'), src_ds):
        pass

    for i in range(1, 4):
      (tmp_dir / f'byte1_{i}.vrt').write_text(
          '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
          '  <VRTRasterBand dataType="Byte" band="1">\n'
          '    <SimpleSource>\n'
          '      <SourceFilename relativeToVRT="1">byte.tif'
          '</SourceFilename>\n'
          '      <SourceBand>1</SourceBand>\n'
          '      <SourceProperties RasterXSize="20" RasterYSize="20"'
          ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
          '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
          '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
          '    </SimpleSource>\n'
          '  </VRTRasterBand>\n'
          '</VRTDataset>',
          encoding='utf-8',
      )

    for level, target in (
        (1, 'byte.tif'),
        (2, 'lvl1.vrt'),
        (3, 'lvl2.vrt'),
        (4, 'lvl3.vrt'),
    ):
      (tmp_dir / f'lvl{level}.vrt').write_text(
          '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
          '  <VRTRasterBand dataType="Byte" band="1">\n'
          '    <SimpleSource>\n'
          f'      <SourceFilename relativeToVRT="1">{target}'
          '</SourceFilename>\n'
          '      <SourceBand>1</SourceBand>\n'
          '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
          '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
          '    </SimpleSource>\n'
          '  </VRTRasterBand>\n'
          '</VRTDataset>',
          encoding='utf-8',
      )

    sources = '\n'.join(
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="1">byte1_{i}.vrt'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>'
        for i in range(1, 4)
    )
    (tmp_dir / 'byte2.vrt').write_text(
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        f'{sources}\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
        encoding='utf-8',
    )

    env = os.environ.copy()
    env['GDAL_MAX_DATASET_POOL_SIZE'] = '3'
    env['VRT_SHARED_SOURCE'] = '0'
    for var in (
        'GUNIT_OUTPUT',
        'TESTBRIDGE_TEST_ONLY',
        'TEST_INFRASTRUCTURE_FAILURE_FILE',
        'TEST_LOGSPLITTER_OUTPUT_FILE',
        'TEST_PREMATURE_EXIT_FILE',
        'TEST_SHARD_STATUS_FILE',
        'TEST_UNUSED_RUNFILES_LOG_FILE',
        'TEST_WARNINGS_OUTPUT_FILE',
        'XML_OUTPUT_FILE',
    ):
      env.pop(var, None)
    cmd = (
        [sys.executable, sys.argv[0], _SMALL_POOL_CHILD_FLAG, str(tmp_dir)]
        if sys.argv[0].endswith('.py')
        else [sys.argv[0], _SMALL_POOL_CHILD_FLAG, str(tmp_dir)]
    )
    proc = subprocess.run(
        cmd, env=env, capture_output=True, text=True, timeout=30, check=False
    )
    self.assertEqual(
        proc.returncode,
        0,
        msg=f'stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}',
    )
    self.assertIn(
        'SMALL_POOL_OK checksum=4672 pool_limit_enforced=1', proc.stdout
    )

  # Upstream: test_vrt_read_21
  def test_implicit_virtual_overviews(self) -> None:
    vsi_dir = self._make_vsi_dir()
    src_tif = str(vsi_dir / 'byte.tif')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      data = src_ds.ReadRaster(0, 0, 20, 20, 400, 400)
    with self.gtiff_driver.Create(src_tif, 400, 400) as ds:
      ds.WriteRaster(0, 0, 400, 400, data)
      ds.BuildOverviews('NEAR', [2])

    vrt_one_band = str(vsi_dir / 'vrt_one_band.vrt')
    gdal.FileFromMemBuffer(
        vrt_one_band,
        '<VRTDataset rasterXSize="800" rasterYSize="800">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="400" RasterYSize="400"'
        ' DataType="Byte" BlockXSize="400" BlockYSize="1" />\n'
        '      <SrcRect xOff="100" yOff="100" xSize="200" ySize="250" />\n'
        '      <DstRect xOff="300" yOff="400" xSize="200" ySize="250" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )
    with gdal.OpenEx(vrt_one_band, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
      data_one_band = ds.ReadRaster(0, 0, 800, 800, 400, 400)

    vrt_two_band = str(vsi_dir / 'vrt_two_band.vrt')
    gdal.FileFromMemBuffer(
        vrt_two_band,
        '<VRTDataset rasterXSize="800" rasterYSize="800">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="400" RasterYSize="400"'
        ' DataType="Byte" BlockXSize="400" BlockYSize="1" />\n'
        '      <SrcRect xOff="100" yOff="100" xSize="200" ySize="250" />\n'
        '      <DstRect xOff="300" yOff="400" xSize="200" ySize="250" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="400" RasterYSize="400"'
        ' DataType="Byte" BlockXSize="400" BlockYSize="1" />\n'
        '      <SrcRect xOff="100" yOff="100" xSize="200" ySize="250" />\n'
        '      <DstRect xOff="300" yOff="400" xSize="200" ySize="250" />\n'
        '      <ScaleOffset>10</ScaleOffset>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )
    with gdal.OpenEx(vrt_two_band, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band1 = ds.GetRasterBand(1)
      band2 = ds.GetRasterBand(2)
      self.assertEqual(band1.GetOverviewCount(), 1)
      self.assertIsNone(band1.GetOverview(-1))
      self.assertIsNone(band1.GetOverview(1))
      ovr1 = band1.GetOverview(0)
      ovr2 = band2.GetOverview(0)
      self.assertIsNotNone(ovr1)
      self.assertIsNotNone(ovr2)
      cs1 = ovr1.Checksum()
      cs2 = ovr2.Checksum()
      ovr1_arr = ovr1.ReadAsArray()
      ovr2_arr = ovr2.ReadAsArray()

      all_data = ds.ReadRaster(0, 0, 800, 800, 400, 400)
      self.assertEqual(
          all_data,
          data_one_band + band2.ReadRaster(0, 0, 800, 800, 400, 400),
      )

      mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
      with mem_drv.Create('', 400, 400, 2) as mem_ds:
        mem_ds.WriteRaster(0, 0, 400, 400, all_data)
        self.assertEqual(cs1, mem_ds.GetRasterBand(1).Checksum())
        self.assertEqual(cs2, mem_ds.GetRasterBand(2).Checksum())
        np.testing.assert_array_equal(
            ovr1_arr, mem_ds.GetRasterBand(1).ReadAsArray()
        )
        np.testing.assert_array_equal(
            ovr2_arr, mem_ds.GetRasterBand(2).ReadAsArray()
        )

      ds.BuildOverviews('NEAR', [2])
      self.assertEqual(cs1, band1.GetOverview(0).Checksum())
      self.assertEqual(cs2, band2.GetOverview(0).Checksum())

  # Upstream: test_vrt_read_virtual_overview_no_srcrect_dstrect
  def test_implicit_virtual_overview_no_srcrect_dstrect(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tmp_tif = str(vsi_dir / 'tmp.tif')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      data = src_ds.ReadRaster(0, 0, 20, 20, 400, 400)
    with self.gtiff_driver.Create(tmp_tif, 400, 400) as ds:
      ds.WriteRaster(0, 0, 400, 400, data)
      ds.BuildOverviews('NEAR', [2])
      ref_ovr = ds.GetRasterBand(1).GetOverview(0)
      ref_cs = ref_ovr.Checksum()
      ref_arr = ref_ovr.ReadAsArray()

    vrt_xml = (
        '<VRTDataset rasterXSize="400" rasterYSize="400">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{tmp_tif}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      self.assertEqual(band.GetOverviewCount(), 1)
      ovr = band.GetOverview(0)
      self.assertEqual(ovr.Checksum(), ref_cs)
      np.testing.assert_array_equal(ovr.ReadAsArray(), ref_arr)

  # Upstream: test_vrt_read_22
  @parameterized.named_parameters(
      dict(
          testcase_name='simple_source',
          source_tag='SimpleSource',
          scale_offset=None,
          expect_min_max_from_src=True,
      ),
      dict(
          testcase_name='complex_source',
          source_tag='ComplexSource',
          scale_offset=None,
          expect_min_max_from_src=True,
      ),
      dict(
          testcase_name='complex_source_with_offset',
          source_tag='ComplexSource',
          scale_offset=10,
          expect_min_max_from_src=False,
      ),
  )
  def test_nbits_honored(
      self,
      source_tag: str,
      scale_offset: int | None,
      expect_min_max_from_src: bool,
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'byte.tif')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds) as copy_ds:
        copy_ds.GetRasterBand(1).ComputeStatistics(False)

    offset_xml = (
        f'      <ScaleOffset>{scale_offset}</ScaleOffset>\n'
        if scale_offset is not None
        else ''
    )
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <Metadata domain="IMAGE_STRUCTURE">\n'
        '      <MDI key="NBITS">6</MDI>\n'
        '    </Metadata>\n'
        f'    <{source_tag}>\n'
        f'      <SourceFilename>{tif_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        f'{offset_xml}'
        f'    </{source_tag}>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      if expect_min_max_from_src:
        self.assertEqual(band.GetMinimum(), 63)
        self.assertEqual(band.GetMaximum(), 63)
      else:
        self.assertIsNone(band.GetMinimum())
        self.assertIsNone(band.GetMaximum())
      self.assertEqual(band.ComputeRasterMinMax(), (63.0, 63.0))
      self.assertEqual(band.ComputeStatistics(False), [63.0, 63.0, 63.0, 0.0])
      np.testing.assert_array_equal(
          band.ReadAsArray(), np.full((20, 20), 63, dtype=np.uint8)
      )

  # Upstream: test_vrt_read_23, test_vrt_read_23a
  @parameterized.named_parameters(
      dict(testcase_name='no_src_nodata', src_nodata=None),
      dict(testcase_name='with_src_nodata', src_nodata=0.0),
  )
  def test_non_nearest_resampling_with_vrt_nodata(
      self, src_nodata: float | None
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'src.tif')
    with self.gtiff_driver.Create(tif_path, 2, 1) as ds:
      if src_nodata is not None:
        ds.GetRasterBand(1).SetNoDataValue(src_nodata)
      ds.GetRasterBand(1).WriteArray(np.array([[0, 10]], dtype=np.uint8))

    vrt_xml = (
        '<VRTDataset rasterXSize="2" rasterYSize="1">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <NoDataValue>0</NoDataValue>\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{tif_path}</SourceFilename>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      got_arr = ds.GetRasterBand(1).ReadAsArray(
          0, 0, 2, 1, 4, 1, resample_alg=gdal.GRIORA_Bilinear
      )
      np.testing.assert_array_equal(
          got_arr, np.array([[0, 10, 10, 10]], dtype=np.uint8)
      )
      self.assertEqual(
          ds.ReadRaster(0, 0, 2, 1, 4, 1, resample_alg=gdal.GRIORA_Bilinear),
          ds.GetRasterBand(1).ReadRaster(
              0, 0, 2, 1, 4, 1, resample_alg=gdal.GRIORA_Bilinear
          ),
      )

  # Upstream: test_vrt_read_24
  def test_zoom_in_no_rounding_gap(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="100" rasterYSize="100">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource resampling="bilinear">\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="11" yOff="20" xSize="60" ySize="60" />\n'
        '    </SimpleSource>\n'
        '    <SimpleSource resampling="bilinear">\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="71" yOff="20" xSize="60" ySize="60" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      data = vrt_ds.ReadRaster(34, 5, 66, 87)
      arr = vrt_ds.GetRasterBand(1).ReadAsArray(34, 5, 66, 87)

    mem_drv = gdal.GetDriverByName(gdrivers_util.MEM_DRIVER)
    with mem_drv.Create('', 66, 87) as mem_ds:
      mem_ds.WriteRaster(0, 0, 66, 87, data)
      # Do not change expected checksum without verifying no black seam at x=71.
      self.assertEqual(mem_ds.GetRasterBand(1).Checksum(), 46612)
    # Column x=71 in VRT coordinates is index 37 in the [34..100) subwindow;
    # rows >= 15 (y >= 20) cover the active destination rectangles.
    self.assertTrue(np.all(arr[15:75, 36:39] > 0))

  # Upstream: test_vrt_read_25
  def test_get_data_coverage_status_multiple_sources(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="2000" rasterYSize="200">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="1000" yOff="30" xSize="10" ySize="20" />\n'
        '    </SimpleSource>\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="1010" yOff="30" xSize="10" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    mixed = gdal.GDAL_DATA_COVERAGE_STATUS_DATA | (
        gdal.GDAL_DATA_COVERAGE_STATUS_EMPTY
    )
    cases = (
        ((0, 0, 20, 20), gdal.GDAL_DATA_COVERAGE_STATUS_DATA, 100.0),
        ((1005, 35, 10, 10), gdal.GDAL_DATA_COVERAGE_STATUS_DATA, 100.0),
        ((100, 100, 20, 20), gdal.GDAL_DATA_COVERAGE_STATUS_EMPTY, 0.0),
        ((10, 10, 20, 20), mixed, 25.0),
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      for window, expected_flags, expected_pct in cases:
        with self.subTest(window=window):
          flags, pct = band.GetDataCoverageStatus(*window)
          self.assertEqual(flags, expected_flags)
          self.assertEqual(pct, expected_pct)

  # Upstream: test_vrt_read_get_data_coverage_status_single_source
  def test_get_data_coverage_status_single_sparse_source(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'sparse.tif')
    vrt_path = str(vsi_dir / 'sparse.vrt')
    with self.gtiff_driver.Create(
        tif_path,
        512,
        512,
        1,
        options=[
            'TILED=YES',
            'BLOCKXSIZE=256',
            'BLOCKYSIZE=256',
            'SPARSE_OK=YES',
        ],
    ) as ds:
      ds.WriteRaster(256, 256, 256, 256, b'\x01' * (256 * 256))

    with gdal.Translate(vrt_path, tif_path, format='VRT'):
      pass

    mixed = gdal.GDAL_DATA_COVERAGE_STATUS_DATA | (
        gdal.GDAL_DATA_COVERAGE_STATUS_EMPTY
    )
    cases = (
        ((0, 0, 512, 512), mixed, 25.0),
        ((0, 0, 256, 256), gdal.GDAL_DATA_COVERAGE_STATUS_EMPTY, 0.0),
        ((256, 256, 256, 256), gdal.GDAL_DATA_COVERAGE_STATUS_DATA, 100.0),
        ((0, 256, 512, 256), mixed, 50.0),
    )
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      for window, expected_flags, expected_pct in cases:
        with self.subTest(window=window):
          flags, pct = band.GetDataCoverageStatus(*window)
          self.assertEqual(flags, expected_flags)
          self.assertEqual(pct, expected_pct)

  # Upstream: test_vrt_read_26
  def test_resampling_subwindow_consistency(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="22" rasterYSize="22">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="22" ySize="22" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      full_arr = band.ReadAsArray(0, 0, 22, 22)
      partial_arr = band.ReadAsArray(1, 1, 1, 1)
    self.assertEqual(int(partial_arr[0, 0]), int(full_arr[1, 1]))

  # Upstream: test_vrt_read_27
  def test_empty_gcplist_opens(self) -> None:
    vrt_xml = (
        '<VRTDataset rasterXSize="1" rasterYSize="1">'
        '<GCPList e=""></GCPList>'
        '<VRTRasterBand></VRTRasterBand>'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.RasterCount, 1)
      self.assertEqual(ds.GetGCPCount(), 0)
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), np.array([[0]], dtype=np.uint8)
      )

  # Upstream: test_vrt_read_28
  def test_invalid_negative_band_number_fails(self) -> None:
    # OSS-Fuzz #1551 malformed attribute spacing + INT32_MIN band regression.
    vsi_dir = self._make_vsi_dir()
    malformed_vrt = str(vsi_dir / 'ossfuzz1551.vrt')
    gdal.FileFromMemBuffer(
        malformed_vrt,
        '<VRTDataset rasterXSize="1 "rasterYSize="1">'
        '<VRTRasterBand band="-2147483648">'
        '<SimpleSource></SimpleSource>'
        '</VRTRasterBand></VRTDataset>',
    )
    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, r'not recognized as being in a supported file format'
    ):
      gdal.OpenEx(malformed_vrt, gdal.OF_RASTER | gdal.OF_READONLY)

    neg_band_vrt = str(vsi_dir / 'neg_band.vrt')
    gdal.FileFromMemBuffer(
        neg_band_vrt,
        '<VRTDataset rasterXSize="1" rasterYSize="1">'
        '<VRTRasterBand band="-2147483648">'
        '<SimpleSource></SimpleSource>'
        '</VRTRasterBand></VRTDataset>',
    )
    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, r'not recognized as being in a supported file format'
    ):
      gdal.OpenEx(neg_band_vrt, gdal.OF_RASTER | gdal.OF_READONLY)

  # Upstream: test_vrt_read_29
  def test_source_file_handle_sharing_and_release(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tif_path = tmp_dir / 'vrt_read_29.tif'
    if _count_open_fds_for_path(tif_path) is None:
      self.skipTest('/proc/<pid>/fd is not readable')

    with gdal.Translate(str(tif_path), gcore_util.GetTestFilePath('byte.tif')):
      pass
    self.assertEqual(_count_open_fds_for_path(tif_path), 0)

    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{tif_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{tif_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )

    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds1:
      self.assertEqual(_count_open_fds_for_path(tif_path), 0)
      self.assertEqual(ds1.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(_count_open_fds_for_path(tif_path), 1)
      # Both bands of ds1 share the same source handle.
      self.assertEqual(ds1.GetRasterBand(2).Checksum(), 4672)
      self.assertEqual(_count_open_fds_for_path(tif_path), 1)

      with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds2:
        # Separate VRT datasets do not share source handles.
        self.assertEqual(ds2.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(_count_open_fds_for_path(tif_path), 2)

      self.assertEqual(_count_open_fds_for_path(tif_path), 1)

    self.assertEqual(_count_open_fds_for_path(tif_path), 0)

  # Upstream: test_vrt_read_30
  def test_dataset_rasterio_empty_bands_pixel_interleaved(self) -> None:
    vrt_xml = (
        '<VRTDataset rasterXSize="2" rasterYSize="2">\n'
        '  <VRTRasterBand dataType="Byte" band="1"></VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2"></VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="3"></VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      data = ds.ReadRaster(
          0,
          0,
          2,
          2,
          2,
          2,
          buf_pixel_space=3,
          buf_line_space=6,
          buf_band_space=1,
      )
    self.assertEqual(data, b'\x00' * 12)

  # Upstream: test_vrt_read_31
  def test_intermediate_data_type_demotion(self) -> None:
    vsi_dir = self._make_vsi_dir()
    asc_path = str(vsi_dir / 'in.asc')
    gdal.FileFromMemBuffer(
        asc_path,
        'ncols        2\n'
        'nrows        2\n'
        'xllcorner    0\n'
        'yllcorner    0\n'
        'dx           1\n'
        'dy           1\n'
        '-255         1\n'
        '254          256',
    )
    with gdal.Translate(
        '', asc_path, outputType=gdal.GDT_Byte, format='VRT'
    ) as ds:
      band_data = struct.unpack(
          'f' * 4,
          ds.GetRasterBand(1).ReadRaster(0, 0, 2, 2, buf_type=gdal.GDT_Float32),
      )
      ds_data = struct.unpack(
          'f' * 4, ds.ReadRaster(0, 0, 2, 2, buf_type=gdal.GDT_Float32)
      )
    self.assertEqual(band_data, (0.0, 1.0, 254.0, 255.0))
    self.assertEqual(ds_data, (0.0, 1.0, 254.0, 255.0))

  # Upstream: test_vrt_float32_with_nodata_slightly_below_float_min
  def test_float32_nodata_slightly_below_float_min_normalized(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'minfloat.tif')
    vrt_path = str(vsi_dir / 'minfloat_nodata.vrt')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('gtiff/minfloat.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      with self.gtiff_driver.CreateCopy(tif_path, src_ds):
        pass

    gdal.FileFromMemBuffer(
        vrt_path,
        '<VRTDataset rasterXSize="4" rasterYSize="1">\n'
        '  <VRTRasterBand dataType="Float32" band="1">\n'
        '    <NoDataValue>-3.402823466385289e+38</NoDataValue>\n'
        '    <ColorInterp>Gray</ColorInterp>\n'
        '    <ComplexSource>\n'
        '      <SourceFilename relativeToVRT="1">minfloat.tif'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <NODATA>-3.402823466385289e+38</NODATA>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      nodata = ds.GetRasterBand(1).GetNoDataValue()
      stats = ds.GetRasterBand(1).ComputeStatistics(False)

    vrt_content = gcore_util.vsi_read_file_as_string(vrt_path, 10000)
    self.assertNotIn('-3.402823466385289', vrt_content)
    self.assertEqual(nodata, -3.4028234663852886e38)
    self.assertEqual(stats, [-3.0, 5.0, 1.0, 4.0])

  # Upstream: test_vrt_subpixel_offset
  def test_subpixel_offset_cubic_mosaic(self) -> None:
    vsi_dir = self._make_vsi_dir()
    left_path = str(vsi_dir / 'left.tif')
    right_top_path = str(vsi_dir / 'right_top.tif')
    right_bottom_path = str(vsi_dir / 'right_bottom.tif')

    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as byte_ds:
      byte_arr = byte_ds.GetRasterBand(1).ReadAsArray()

    with self.gtiff_driver.Create(left_path, 10, 20) as ds:
      ds.GetRasterBand(1).WriteArray(byte_arr[:, :10])
    with self.gtiff_driver.Create(right_top_path, 10, 10) as ds:
      ds.GetRasterBand(1).WriteArray(byte_arr[:10, 10:])
    with self.gtiff_driver.Create(right_bottom_path, 10, 10) as ds:
      ds.GetRasterBand(1).WriteArray(byte_arr[10:, 10:])

    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource resampling="cubic">\n'
        f'      <SourceFilename relativeToVRT="0">{left_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="10" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="10" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="10" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="10" ySize="20" />\n'
        '    </SimpleSource>\n'
        '    <SimpleSource resampling="cubic">\n'
        f'      <SourceFilename relativeToVRT="0">{right_top_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="10" RasterYSize="10"'
        ' DataType="Byte" BlockXSize="10" BlockYSize="10" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10" />\n'
        '      <DstRect xOff="9.5" yOff="0" xSize="10" ySize="10" />\n'
        '    </SimpleSource>\n'
        '    <SimpleSource resampling="cubic">\n'
        f'      <SourceFilename relativeToVRT="0">{right_bottom_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="10" RasterYSize="10"'
        ' DataType="Byte" BlockXSize="10" BlockYSize="10" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10" />\n'
        '      <DstRect xOff="9.5" yOff="9.5" xSize="10" ySize="10" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      checksum = band.Checksum()
      arr = band.ReadAsArray()
    self.assertEqual(arr.shape, (20, 20))
    # Synthesized byte.tif-slice tiles produce 4943 (upstream fixtures: 4849).
    self.assertEqual(checksum, 4943)
    np.testing.assert_array_equal(arr[:, :9], byte_arr[:, :9])
    self.assertFalse(np.array_equal(arr[:, 10:], byte_arr[:, 10:]))

  # Upstream: test_vrt_dstsize_larger_than_source
  def test_dstsize_larger_than_source(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'src.tif')
    with self.gtiff_driver.Create(
        tif_path, 528, 528, 1, gdal.GDT_Byte, options=['COMPRESS=DEFLATE']
    ) as src_ds:
      src_ds.GetRasterBand(1).Fill(255)

    vrt_xml = (
        '<VRTDataset rasterXSize="3476" rasterYSize="5828">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Gray</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename relativeToVRT="0">{tif_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="528" RasterYSize="528"'
        ' DataType="Byte" BlockXSize="528" BlockYSize="15" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="528" ySize="528" />\n'
        '      <DstRect xOff="27.9291767207669" yOff="4736.24196374206"'
        ' xSize="528.428209166479" ySize="528.428209166479" />\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      band = vrt_ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 33273)
      sub = band.ReadAsArray(26, 4735, 532, 532)
    self.assertTrue(np.all(sub[2:529, 2:529] == 255))
    self.assertTrue(np.all(sub[0, :] == 0))

  # Upstream: test_vrt_invalid_srcrect, test_vrt_invalid_dstrect
  @parameterized.named_parameters(
      dict(
          testcase_name='invalid_srcrect',
          src_rect='xOff="0" yOff="0" xSize="-10" ySize="20"',
          dst_rect='xOff="0" yOff="0" xSize="20" ySize="20"',
          expected_error=r'Wrong values in SrcRect',
      ),
      dict(
          testcase_name='invalid_dstrect',
          src_rect='xOff="0" yOff="0" xSize="20" ySize="20"',
          dst_rect='xOff="0" yOff="0" xSize="20" ySize="1e400"',
          expected_error=r'Wrong values in DstRect',
      ),
  )
  def test_invalid_source_or_dst_rect_fails(
      self, src_rect: str, dst_rect: str, expected_error: str
  ) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        f'      <SrcRect {src_rect} />\n'
        f'      <DstRect {dst_rect} />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, expected_error
    ):
      gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY)

  # Upstream: test_vrt_no_explicit_dataAxisToSRSAxisMapping,
  # test_vrt_explicit_dataAxisToSRSAxisMapping_1_2
  @parameterized.named_parameters(
      dict(
          testcase_name='default_epsg_4326',
          srs_attr='',
          expected_mapping=[2, 1],
      ),
      dict(
          testcase_name='explicit_1_2',
          srs_attr=' dataAxisToSRSAxisMapping="1,2"',
          expected_mapping=[1, 2],
      ),
  )
  def test_srs_axis_mapping(
      self, srs_attr: str, expected_mapping: list[int]
  ) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    wkt_4326 = (
        'GEOGCS["WGS 84",DATUM["WGS_1984",'
        'SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],'
        'AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,'
        'AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,'
        'AUTHORITY["EPSG","9122"]],AXIS["Latitude",NORTH],'
        'AXIS["Longitude",EAST],AUTHORITY["EPSG","4326"]]'
    )
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        f'  <SRS{srs_attr}>{wkt_4326}</SRS>\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          ds.GetSpatialRef().GetDataAxisToSRSAxisMapping(), expected_mapping
      )

  # Upstream: test_vrt_shared_no_proxy_pool
  def test_shared_sources_release_file_handles(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    rgb_copy = tmp_dir / 'rgbsmall.tif'
    if _count_open_fds_for_path(rgb_copy) is None:
      self.skipTest('/proc/<pid>/fd is not readable')

    with gdal.Translate(
        str(rgb_copy), gcore_util.GetTestFilePath('rgbsmall.tif')
    ):
      pass
    self.assertEqual(_count_open_fds_for_path(rgb_copy), 0)

    vrt_xml = (
        '<VRTDataset rasterXSize="50" rasterYSize="50">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Red</ColorInterp>\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{rgb_copy}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <ColorInterp>Green</ColorInterp>\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{rgb_copy}</SourceFilename>\n'
        '      <SourceBand>2</SourceBand>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="3">\n'
        '    <ColorInterp>Blue</ColorInterp>\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{rgb_copy}</SourceFilename>\n'
        '      <SourceBand>3</SourceBand>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 21212)
      self.assertEqual(ds.GetRasterBand(2).Checksum(), 21053)
      self.assertEqual(ds.GetRasterBand(3).Checksum(), 21349)

    self.assertEqual(_count_open_fds_for_path(rgb_copy), 0)

  # Upstream: test_vrt_invalid_source_band
  def test_invalid_source_band_raises(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="50" rasterYSize="50">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename>{byte_path}</SourceFilename>\n'
        '      <SourceBand>10</SourceBand>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError, r'Illegal band #|I/O read error'
      ):
        ds.GetRasterBand(1).Checksum()

  # Upstream: test_vrt_protocol (invalid specs & query validation)
  @parameterized.named_parameters(
      dict(testcase_name='empty', spec='vrt://'),
      dict(testcase_name='missing_file', spec='vrt://i_do_not_exist'),
      dict(testcase_name='missing_file_query', spec='vrt://i_do_not_exist?'),
  )
  def test_vrt_protocol_missing_path(self, spec: str) -> None:
    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, r'No such file or directory'
    ):
      gdal.OpenEx(spec, gdal.OF_RASTER | gdal.OF_READONLY)

  # Upstream: test_vrt_protocol (error parameter validation)
  @parameterized.named_parameters(
      dict(
          testcase_name='unknown_key',
          query='foo=bar',
          expected_error=r'Unknown option: foo',
      ),
      dict(
          testcase_name='bands_non_numeric',
          query='bands=foo',
          expected_error=r'Invalid band number: foo',
      ),
      dict(
          testcase_name='bands_zero',
          query='bands=0',
          expected_error=r'Invalid band number: 0',
      ),
      dict(
          testcase_name='bands_out_of_range',
          query='bands=2',
          expected_error=r'Invalid band number: 2',
      ),
      dict(
          testcase_name='gcp_invalid',
          query='gcp=invalid',
          expected_error=r'Invalid value for GCP',
      ),
      dict(
          testcase_name='if_missing_driver',
          query='if=AAIGrid,doesnotexist',
          expected_error=r'not recognized|Unable to open',
      ),
      dict(
          testcase_name='if_repeated',
          query='if=AAIGrid&if=GTiff',
          expected_error=r"'if' option should be specified once",
      ),
      dict(
          testcase_name='exponent_without_scale',
          query='exponent=2.2',
          expected_error=r'-scale should be specified when -exponent',
      ),
      dict(
          testcase_name='outsize_single',
          query='outsize=10',
          expected_error=r'Invalid outsize option',
      ),
      dict(
          testcase_name='projwin_3_args',
          query='projwin=440840,441920,3750120',
          expected_error=r'Invalid projwin option',
      ),
      dict(
          testcase_name='tr_single',
          query='tr=120',
          expected_error=r'Invalid tr option',
      ),
      dict(
          testcase_name='srcwin_3_args',
          query='srcwin=0,0,3',
          expected_error=r'Invalid srcwin option',
      ),
      dict(
          testcase_name='a_gt_5_args',
          query='a_gt=1,0,0,0,1',
          expected_error=r'Invalid a_gt option',
      ),
      dict(
          testcase_name='scale_no_value',
          query='scale',
          expected_error=r'Invalid option specification',
      ),
      dict(
          testcase_name='unscale_no_value',
          query='a_ullr=0,1,1,0&unscale&',
          expected_error=r'Invalid option specification',
      ),
      dict(
          testcase_name='epo_oob',
          query='srcwin=0,0,20,21&epo=true',
          expected_error=r'outside source raster extent',
      ),
      dict(
          testcase_name='eco_oob',
          query='srcwin=20,20,1,1&epo=false&eco=true',
          expected_error=r'outside source raster extent',
      ),
  )
  def test_vrt_protocol_invalid_options(
      self, query: str, expected_error: str
  ) -> None:
    filepath = gcore_util.GetTestFilePath('float32.tif')
    self.assertTrue(pathlib.Path(filepath).exists())
    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, expected_error
    ):
      gdal.OpenEx(
          f'vrt://{filepath}?{query}', gdal.OF_RASTER | gdal.OF_READONLY
      )

  # Upstream: test_vrt_protocol (core options on byte/int32/float32/gcps)
  def test_vrt_protocol_core_options(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    int32_path = gcore_util.GetTestFilePath('int32.tif')
    float32_path = gcore_util.GetTestFilePath('float32.tif')
    uint16_3band_path = gcore_util.GetTestFilePath('uint16_3band.vrt')
    minfloat_path = gcore_util.GetTestFilePath('gtiff/minfloat.tif')
    gcps_path = gcore_util.GetTestFilePath('gcps.vrt')

    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    fd_tif = tmp_dir / 'fd_check.tif'
    fd_tif.write_bytes(pathlib.Path(byte_path).read_bytes())
    if _count_open_fds_for_path(fd_tif) is not None:
      self.assertEqual(_count_open_fds_for_path(fd_tif), 0)
      with gdal.OpenEx(
          f'vrt://{fd_tif}?bands=1,mask', gdal.OF_RASTER | gdal.OF_READONLY
      ) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(_count_open_fds_for_path(fd_tif), 0)

    with gdal.OpenEx(
        f'vrt://{byte_path}', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.RasterCount, 1)
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

    with gdal.OpenEx(
        f'vrt://{byte_path}?bands=1,mask,1', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.RasterCount, 3)
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(ds.GetRasterBand(2).Checksum(), 4873)
      self.assertEqual(ds.GetRasterBand(3).Checksum(), 4672)

    with gdal.OpenEx(
        f'vrt://{byte_path}?bands=1&a_srs=EPSG:3031',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), '3031')

    with gdal.OpenEx(
        f'vrt://{byte_path}?a_ullr=0,10,20,0', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetGeoTransform(), (0.0, 1.0, 0.0, 10.0, 0.0, -0.5))

    with gdal.OpenEx(
        f'vrt://{int32_path}?a_scale=2&a_offset=-132',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetScale(), 2.0)
      self.assertEqual(ds.GetRasterBand(1).GetOffset(), -132.0)

    with gdal.OpenEx(
        f'vrt://{float32_path}?ot=Int32', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Int32)

    with gdal.OpenEx(
        f'vrt://{float32_path}?gcp=0,0,0,10,0',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetGCPCount(), 1)

    with gdal.OpenEx(
        f'vrt://{float32_path}?gcp=0,0,0,10,0&gcp=20,0,10,10,0',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetGCPCount(), 2)

    with gdal.OpenEx(
        f'vrt://{float32_path}?gcp=0,0,0,10&gcp=20,0,10,10',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetGCPCount(), 2)

    with gdal.OpenEx(
        f'vrt://{float32_path}?if=AAIGrid,GTiff',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.RasterXSize, 20)

    with gdal.OpenEx(
        f'vrt://{float32_path}?scale=0,255,255,255',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4873)

    with gdal.OpenEx(
        f'vrt://{uint16_3band_path}?scale_2=0,255,255,255',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetRasterBand(2).Checksum(), 5047)

    with gdal.OpenEx(
        f'vrt://{float32_path}?scale_1=0,10', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4151)

    with gdal.OpenEx(
        f'vrt://{float32_path}?exponent=2.2&scale=0,100',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 400)

    with gdal.OpenEx(
        f'vrt://{uint16_3band_path}?exponent_2=2.2&scale_2=0,10,0,100',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetRasterBand(2).Checksum(), 4455)

    for query in ('outsize=10,5', 'outsize=50%,25%'):
      with self.subTest(query=query):
        with gdal.OpenEx(
            f'vrt://{float32_path}?{query}', gdal.OF_RASTER | gdal.OF_READONLY
        ) as ds:
          self.assertEqual(ds.GetRasterBand(1).XSize, 10)
          self.assertEqual(ds.GetRasterBand(1).YSize, 5)

    with gdal.OpenEx(
        f'vrt://{float32_path}?projwin=440840,3751080,441920,3750120',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetGeoTransform()[0], 440840.0)
      self.assertEqual(ds.GetGeoTransform()[3], 3751080.0)
      self.assertEqual(ds.GetRasterBand(1).XSize, 18)
      self.assertEqual(ds.GetRasterBand(1).YSize, 16)

    with gdal.OpenEx(
        f'vrt://{float32_path}?projwin_srs=OGC:CRS84',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.RasterXSize, 20)

    with gdal.OpenEx(
        f'vrt://{float32_path}?projwin_srs=OGC:CRS84'
        '&projwin=-117.6407,33.90027,-117.6292,33.89181',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetGeoTransform()[0], 440780.0)
      self.assertEqual(ds.GetGeoTransform()[3], 3751140.0)
      self.assertEqual(ds.GetRasterBand(1).XSize, 19)
      self.assertEqual(ds.GetRasterBand(1).YSize, 17)

    with gdal.OpenEx(
        f'vrt://{float32_path}?tr=120,240', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      gt = ds.GetGeoTransform()
      self.assertEqual(gt[0], 440720.0)
      self.assertEqual(gt[1], 120.0)
      self.assertEqual(gt[5], -240.0)
      self.assertEqual(ds.GetRasterBand(1).XSize, 10)
      self.assertEqual(ds.GetRasterBand(1).YSize, 5)

    with gdal.OpenEx(
        f'vrt://{float32_path}?r=bilinear&tr=120,240',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      val = float(ds.GetRasterBand(1).ReadAsArray(0, 0, 1, 1)[0, 0])
      self.assertAlmostEqual(val, 128.95408630371094, places=4)

    with gdal.OpenEx(
        f'vrt://{float32_path}?srcwin=2,3,8,5',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).XSize, 8)
      self.assertEqual(ds.GetRasterBand(1).YSize, 5)
      val = float(ds.GetRasterBand(1).ReadAsArray(0, 0, 1, 1)[0, 0])
      self.assertAlmostEqual(val, 123.0)

    with gdal.OpenEx(
        f'vrt://{float32_path}?a_gt=0,1,0,0,0,1&a_nodata=-9999',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      np.testing.assert_allclose(
          ds.GetGeoTransform(), (0.0, 1.0, 0.0, 0.0, 0.0, 1.0), atol=1e-9
      )
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), -9999.0)

    with gdal.OpenEx(
        f'vrt://{minfloat_path}?scale=true', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      arr = ds.GetRasterBand(1).ReadAsArray(2, 0, 1, 1)
      self.assertAlmostEqual(float(arr[0, 0]), 1.0)

    with gdal.OpenEx(
        f'vrt://{minfloat_path}?scale_2=true&bands=1,1',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertAlmostEqual(
          float(ds.GetRasterBand(2).ReadAsArray(2, 0, 1, 1)[0, 0]), 1.0
      )
      self.assertAlmostEqual(
          float(ds.GetRasterBand(1).ReadAsArray(2, 0, 1, 1)[0, 0]), 5.0
      )

    with gdal.OpenEx(
        f'vrt://{gcps_path}?nogcp=true', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetGCPCount(), 0)

    with gdal.OpenEx(
        f'vrt://{byte_path}?srcwin=0,0,20,20&eco=true&epo=true',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.RasterXSize, 20)

    with gdal.OpenEx(
        f'vrt://{byte_path}?srcwin=0,0,20,21&eco=true',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.RasterYSize, 21)

  # Upstream: test_vrt_protocol (overviews, open options, unscale, subdatasets)
  def test_vrt_protocol_overviews_open_options_unscale_and_subdatasets(
      self,
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    byte_path = gcore_util.GetTestFilePath('byte.tif')

    byte_ovr_path = str(vsi_dir / 'byte_with_ovr.tif')
    with gdal.OpenEx(byte_path, gdal.OF_RASTER | gdal.OF_READONLY) as src_ds:
      with self.gtiff_driver.CreateCopy(byte_ovr_path, src_ds) as ovr_ds:
        ovr_ds.BuildOverviews('NEAR', [2, 4])

    with gdal.OpenEx(
        f'vrt://{byte_ovr_path}?ovr=0', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.RasterXSize, 10)
      self.assertEqual(ds.RasterYSize, 10)

    with gdal.OpenEx(
        f'vrt://{byte_ovr_path}?oo=GEOREF_SOURCES=TABFILE,OVERVIEW_LEVEL=0',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      np.testing.assert_allclose(
          ds.GetGeoTransform(), (0.0, 1.0, 0.0, 0.0, 0.0, 1.0), atol=1e-9
      )
      self.assertEqual(ds.RasterXSize, 10)
      self.assertEqual(ds.RasterYSize, 10)

    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, r'option should be specified once'
    ):
      gdal.OpenEx(
          f'vrt://{byte_ovr_path}'
          '?oo=GEOREF_SOURCES=TABFILE&oo=OVERVIEW_LEVEL=0',
          gdal.OF_RASTER | gdal.OF_READONLY,
      )

    unscale_tif = str(vsi_dir / 'scale2.tif')
    with gdal.Translate(unscale_tif, f'vrt://{byte_path}?a_scale=2'):
      pass
    with gdal.OpenEx(
        f'vrt://{unscale_tif}?unscale=true&ot=int16',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(
          int(ds.GetRasterBand(1).ReadAsArray(0, 0, 1, 1)[0, 0]), 214
      )

    # Multi-directory TIFF subdatasets: page 1 has byte.tif, page 2 is zeros.
    multi_tif = str(vsi_dir / 'tiff_with_subifds.tif')
    with gdal.OpenEx(byte_path, gdal.OF_RASTER | gdal.OF_READONLY) as src_ds:
      with self.gtiff_driver.CreateCopy(multi_tif, src_ds):
        pass
    with self.gtiff_driver.Create(
        multi_tif, 10, 10, 1, gdal.GDT_Byte, options=['APPEND_SUBDATASET=YES']
    ) as page2:
      page2.GetRasterBand(1).Fill(0)

    with gdal.OpenEx(
        f'vrt://{multi_tif}?sd=2', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), np.zeros((10, 10), dtype=np.uint8)
      )

    with gdal.OpenEx(
        f'vrt://{multi_tif}?sd_name=1', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

    bad_subdataset_queries = (
        ('sd=2&sd_name=1', r"'sd_name' is mutually exclusive with option 'sd'"),
        ('sd=3', r"'sd' option should indicate a valid subdataset component"),
        (
            'sd_name=sds',
            r"'sd_name' option should .*valid subdataset component",
        ),
    )
    for bad_query, expected_error in bad_subdataset_queries:
      with self.subTest(query=bad_query):
        with gdal.quiet_errors(), self.assertRaisesRegex(
            RuntimeError, expected_error
        ):
          gdal.OpenEx(
              f'vrt://{multi_tif}?{bad_query}',
              gdal.OF_RASTER | gdal.OF_READONLY,
          )

  # Upstream: test_vrt_protocol_netcdf_component_name
  def test_vrt_protocol_netcdf_component_name(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    nc_path = str(tmp_dir / 'multi.nc')
    nc_drv = gdal.GetDriverByName(gdrivers_util.NETCDF_DRIVER)
    self.assertIsNotNone(nc_drv)

    with nc_drv.CreateMultiDimensional(nc_path) as nc_ds:
      root = nc_ds.GetRootGroup()
      dim_y = root.CreateDimension('y', 'HORIZONTAL_Y', None, 2)
      dim_x = root.CreateDimension('x', 'HORIZONTAL_X', None, 3)
      arr1 = root.CreateMDArray(
          'var_one', [dim_y, dim_x], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      arr1.Write(np.array([[1, 2, 3], [4, 5, 6]], dtype=np.uint8))
      arr2 = root.CreateMDArray(
          'var_two', [dim_y, dim_x], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      expected_two = np.array([[10, 20, 30], [40, 50, 60]], dtype=np.uint8)
      arr2.Write(expected_two)

    with gdal.OpenEx(
        f'vrt://{nc_path}?sd_name=var_two', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.RasterXSize, 3)
      self.assertEqual(ds.RasterYSize, 2)
      # Without a descending Y coordinate variable, GDAL's NetCDF driver reads
      # 2D arrays bottom-up (CF convention).
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), expected_two[::-1]
      )

  # Upstream: test_vrt_protocol_a_coord_epoch_option
  def test_vrt_protocol_a_coord_epoch_option(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    with gdal.OpenEx(
        f'vrt://{byte_path}?a_srs=EPSG:4326&a_coord_epoch=2021.3',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      srs = ds.GetSpatialRef()
      self.assertTrue(srs.IsDynamic())
      self.assertEqual(srs.GetCoordinateEpoch(), 2021.3)

  # Upstream: test_vrt_protocol_expand_option
  def test_vrt_protocol_expand_option(self) -> None:
    # Upstream uses 8bit_pal.bmp, but BMP is not built in Google3; synthesize a
    # paletted GTiff in /vsimem/ so ?expand=rgb is actually exercised.
    vsi_dir = self._make_vsi_dir()
    pal_tif = str(vsi_dir / 'paletted.tif')
    ct = gdal.ColorTable()
    ct.SetColorEntry(0, (10, 20, 30, 255))
    ct.SetColorEntry(1, (100, 150, 200, 255))
    with self.gtiff_driver.Create(pal_tif, 2, 1, 1, gdal.GDT_Byte) as ds:
      band = ds.GetRasterBand(1)
      band.SetColorInterpretation(gdal.GCI_PaletteIndex)
      band.SetColorTable(ct)
      band.WriteArray(np.array([[0, 1]], dtype=np.uint8))

    with gdal.OpenEx(
        f'vrt://{pal_tif}?expand=rgb', gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.RasterCount, 3)
      self.assertEqual(
          ds.GetRasterBand(1).GetRasterColorInterpretation(), gdal.GCI_RedBand
      )
      self.assertEqual(
          ds.GetRasterBand(2).GetRasterColorInterpretation(), gdal.GCI_GreenBand
      )
      self.assertEqual(
          ds.GetRasterBand(3).GetRasterColorInterpretation(), gdal.GCI_BlueBand
      )
      np.testing.assert_array_equal(
          ds.ReadAsArray(),
          np.array([[[10, 100]], [[20, 150]], [[30, 200]]], dtype=np.uint8),
      )

  # Upstream: test_vrt_protocol_transpose_option
  def test_vrt_protocol_transpose_option(self) -> None:
    vsi_dir = self._make_vsi_dir()
    zarr_path = str(vsi_dir / 'mdim.zarr')
    zarr_drv = gdal.GetDriverByName(gdrivers_util.ZARR_DRIVER)
    self.assertIsNotNone(zarr_drv)

    with zarr_drv.CreateMultiDimensional(zarr_path) as mdim_ds:
      root = mdim_ds.GetRootGroup()
      d0 = root.CreateDimension('d0', None, None, 2)
      d1 = root.CreateDimension('d1', None, None, 3)
      d2 = root.CreateDimension('d2', None, None, 4)
      byte_edt = gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      arr = root.CreateMDArray('MyDataField', [d0, d1, d2], byte_edt)
      arr.Write(np.arange(24, dtype=np.uint8).reshape(2, 3, 4))
      other = root.CreateMDArray('OtherField', [d0, d1], byte_edt)
      other.Write(np.zeros((2, 3), dtype=np.uint8))

    with gdal.OpenEx(
        f'vrt://{zarr_path}?transpose=/MyDataField:0,1',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.RasterXSize, 2)
      self.assertEqual(ds.RasterYSize, 3)

    with gdal.OpenEx(
        f'vrt://{zarr_path}?transpose=/MyDataField:2,1',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as ds:
      self.assertEqual(ds.RasterXSize, 4)
      self.assertEqual(ds.RasterYSize, 3)

    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError,
        r"'sd_name' is mutually exclusive with option 'transpose'",
    ):
      gdal.OpenEx(
          f'vrt://{zarr_path}?sd_name=MyDataField&transpose=/MyDataField:1,0',
          gdal.OF_RASTER | gdal.OF_READONLY,
      )

    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError,
        r"'sd' is mutually exclusive with option 'transpose'",
    ):
      gdal.OpenEx(
          f'vrt://{zarr_path}?sd=1&transpose=/MyDataField:1,0',
          gdal.OF_RASTER | gdal.OF_READONLY,
      )

  # Upstream: test_vrt_source_no_dstrect
  def test_source_without_dstrect(self) -> None:
    vsi_dir = self._make_vsi_dir()
    out_path = str(vsi_dir / 'out.tif')
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.Translate(out_path, vrt_xml):
      pass
    with gdal.OpenEx(byte_path, gdal.OF_RASTER | gdal.OF_READONLY) as byte_ds:
      expected_arr = byte_ds.GetRasterBand(1).ReadAsArray()
    with gdal.OpenEx(out_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), expected_arr
      )

  # Upstream: test_vrt_dataset_rasterio_recursion_detection
  def test_dataset_rasterio_overview_recursion_detection(self) -> None:
    vsi_dir = self._make_vsi_dir()
    vrt_path = str(vsi_dir / 'test.vrt')
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    gdal.FileFromMemBuffer(
        vrt_path,
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '    <Overview>\n'
        f'      <SourceFilename relativeToVRT="0">{vrt_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </Overview>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with gdal.quiet_errors():
        vrt_values = ds.ReadRaster(0, 0, 20, 20, 10, 10)
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as byte_ds:
      tif_values = byte_ds.ReadRaster(0, 0, 20, 20, 10, 10)
    self.assertEqual(vrt_values, tif_values)

    cycle_a = str(vsi_dir / 'cycle_a.vrt')
    cycle_b = str(vsi_dir / 'cycle_b.vrt')
    for src, target in ((cycle_a, cycle_b), (cycle_b, cycle_a)):
      gdal.FileFromMemBuffer(
          src,
          '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
          '  <VRTRasterBand dataType="Byte" band="1">\n'
          '    <SimpleSource>\n'
          f'      <SourceFilename relativeToVRT="0">{target}'
          '</SourceFilename>\n'
          '      <SourceBand>1</SourceBand>\n'
          '    </SimpleSource>\n'
          '  </VRTRasterBand>\n'
          '</VRTDataset>',
      )
    with gdal.OpenEx(cycle_a, gdal.OF_RASTER | gdal.OF_READONLY) as cycle_ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError, r'Recursion detected'
      ):
        cycle_ds.ReadRaster(0, 0, 20, 20, 10, 10)

  # Upstream: test_vrt_dataset_rasterio_recursion_detection_does_not_trigger
  def test_dataset_rasterio_cubic_no_false_recursion(self) -> None:
    rgb_path = gcore_util.GetTestFilePath('rgbsmall.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="50" rasterYSize="50">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Red</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{rgb_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <ColorInterp>Green</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{rgb_path}</SourceFilename>\n'
        '      <SourceBand>2</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="3">\n'
        '    <ColorInterp>Blue</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{rgb_path}</SourceFilename>\n'
        '      <SourceBand>3</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      got_arr = vrt_ds.ReadAsArray(
          0,
          0,
          50,
          50,
          buf_xsize=25,
          buf_ysize=25,
          resample_alg=gdal.GRIORA_Cubic,
      )
    with gdal.OpenEx(rgb_path, gdal.OF_RASTER | gdal.OF_READONLY) as ref_ds:
      ref_arr = ref_ds.ReadAsArray(
          0,
          0,
          50,
          50,
          buf_xsize=25,
          buf_ysize=25,
          resample_alg=gdal.GRIORA_Cubic,
      )
    np.testing.assert_array_equal(got_arr, ref_arr)

  # Upstream: test_vrt_dataset_rasterio_non_nearest_resampling_source_with_ovr
  def test_dataset_rasterio_non_nearest_source_with_overviews(self) -> None:
    vsi_dir = self._make_vsi_dir()
    src_tif = str(vsi_dir / 'src.tif')
    with self.gtiff_driver.Create(src_tif, 10, 10, 3) as ds:
      ds.GetRasterBand(1).Fill(255)
      ds.BuildOverviews('NONE', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(10)

    vrt_xml = (
        '<VRTDataset rasterXSize="10" rasterYSize="10">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Red</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="10" ySize="5" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="10" ySize="5" />\n'
        '    </ComplexSource>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="5" xSize="10" ySize="5" />\n'
        '      <DstRect xOff="0" yOff="5" xSize="10" ySize="5" />\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <ColorInterp>Green</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>2</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="3">\n'
        '    <ColorInterp>Blue</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src_tif}</SourceFilename>\n'
        '      <SourceBand>3</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      arr_nearest = ds.ReadAsArray(0, 0, 10, 10, buf_xsize=4, buf_ysize=4)
      self.assertEqual(int(arr_nearest[0, 0, 0]), 10)
      arr_cubic = ds.ReadAsArray(
          0,
          0,
          10,
          10,
          buf_xsize=4,
          buf_ysize=4,
          resample_alg=gdal.GRIORA_Cubic,
      )
      self.assertEqual(int(arr_cubic[0, 0, 0]), 10)

  # Upstream: test_vrt_implicit_ovr_with_hidenodatavalue
  def test_implicit_ovr_with_hide_nodata_value(self) -> None:
    vsi_dir = self._make_vsi_dir()
    src_tif = str(vsi_dir / 'src.tif')
    with self.gtiff_driver.Create(src_tif, 256, 256, 3) as ds:
      ds.GetRasterBand(1).Fill(255)
      ds.BuildOverviews('NONE', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(10)

    bands_xml = []
    for idx, interp in ((1, 'Red'), (2, 'Green'), (3, 'Blue')):
      bands_xml.append(
          f'  <VRTRasterBand dataType="Byte" band="{idx}">\n'
          f'    <ColorInterp>{interp}</ColorInterp>\n'
          '    <NoDataValue>5</NoDataValue>\n'
          '    <HideNoDataValue>1</HideNoDataValue>\n'
          '    <ComplexSource>\n'
          f'      <SourceFilename>{src_tif}</SourceFilename>\n'
          f'      <SourceBand>{idx}</SourceBand>\n'
          '      <SrcRect xOff="0" yOff="0" xSize="128" ySize="128" />\n'
          '      <DstRect xOff="128" yOff="128" xSize="128" ySize="128" />\n'
          '    </ComplexSource>\n'
          '  </VRTRasterBand>'
      )
    vrt_xml = (
        '<VRTDataset rasterXSize="256" rasterYSize="256">\n'
        + '\n'.join(bands_xml)
        + '\n</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
      ds_arr = ds.ReadAsArray(0, 0, 256, 256, buf_xsize=64, buf_ysize=64)
      self.assertEqual(int(ds_arr[0, 0, 0]), 5)
      self.assertEqual(int(ds_arr[0, 32, 32]), 10)
      band_arr = ds.GetRasterBand(1).ReadAsArray(
          0, 0, 256, 256, buf_xsize=64, buf_ysize=64
      )
      self.assertEqual(int(band_arr[0, 0]), 5)
      self.assertEqual(int(band_arr[32, 32]), 10)

  # Upstream: test_vrt_usemaskband
  def test_use_mask_band(self) -> None:
    vsi_dir = self._make_vsi_dir()
    src1 = str(vsi_dir / 'src1.tif')
    src2 = str(vsi_dir / 'src2.tif')
    with self.gtiff_driver.Create(src1, 3, 1) as ds:
      ds.GetRasterBand(1).Fill(255)
      with gdal.config_option('GDAL_TIFF_INTERNAL_MASK', 'NO'):
        ds.CreateMaskBand(0)
      ds.GetRasterBand(1).GetMaskBand().WriteRaster(0, 0, 1, 1, b'\xff')

    with self.gtiff_driver.Create(src2, 3, 1) as ds:
      ds.GetRasterBand(1).Fill(127)
      with gdal.config_option('GDAL_TIFF_INTERNAL_MASK', 'NO'):
        ds.CreateMaskBand(0)
      ds.GetRasterBand(1).GetMaskBand().WriteRaster(1, 0, 1, 1, b'\xff')

    vrt_xml = (
        '<VRTDataset rasterXSize="3" rasterYSize="1">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src1}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <UseMaskBand>true</UseMaskBand>\n'
        '    </ComplexSource>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src2}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <UseMaskBand>true</UseMaskBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <MaskBand>\n'
        '    <VRTRasterBand dataType="Byte">\n'
        '      <ComplexSource>\n'
        f'        <SourceFilename>{src1}</SourceFilename>\n'
        '        <SourceBand>mask,1</SourceBand>\n'
        '        <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '        <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '        <UseMaskBand>true</UseMaskBand>\n'
        '      </ComplexSource>\n'
        '      <ComplexSource>\n'
        f'        <SourceFilename>{src2}</SourceFilename>\n'
        '        <SourceBand>mask,1</SourceBand>\n'
        '        <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '        <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '        <UseMaskBand>true</UseMaskBand>\n'
        '      </ComplexSource>\n'
        '    </VRTRasterBand>\n'
        '  </MaskBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(),
          np.array([[255, 127, 0]], dtype=np.uint8),
      )
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).GetMaskBand().ReadAsArray(),
          np.array([[255, 255, 0]], dtype=np.uint8),
      )

  # Upstream: test_vrt_usemaskband_alpha
  def test_use_mask_band_alpha(self) -> None:
    vsi_dir = self._make_vsi_dir()
    src1 = str(vsi_dir / 'src1.tif')
    src2 = str(vsi_dir / 'src2.tif')
    with self.gtiff_driver.Create(src1, 3, 1, 2) as ds:
      ds.GetRasterBand(1).Fill(255)
      ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_AlphaBand)
      ds.GetRasterBand(2).WriteRaster(0, 0, 1, 1, b'\xff')

    with self.gtiff_driver.Create(src2, 3, 1, 2) as ds:
      ds.GetRasterBand(1).Fill(127)
      ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_AlphaBand)
      ds.GetRasterBand(2).WriteRaster(1, 0, 1, 1, b'\xff')

    vrt_xml = (
        '<VRTDataset rasterXSize="3" rasterYSize="1">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src1}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <UseMaskBand>true</UseMaskBand>\n'
        '    </ComplexSource>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src2}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <UseMaskBand>true</UseMaskBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <VRTRasterBand dataType="Byte" band="2">\n'
        '    <ColorInterp>Alpha</ColorInterp>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src1}</SourceFilename>\n'
        '      <SourceBand>2</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <UseMaskBand>true</UseMaskBand>\n'
        '    </ComplexSource>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename>{src2}</SourceFilename>\n'
        '      <SourceBand>2</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="3" ySize="1" />\n'
        '      <UseMaskBand>true</UseMaskBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(),
          np.array([[255, 127, 0]], dtype=np.uint8),
      )
      np.testing.assert_array_equal(
          ds.GetRasterBand(2).ReadAsArray(),
          np.array([[255, 255, 0]], dtype=np.uint8),
      )

  # Upstream: test_vrt_check_dont_open_unneeded_source,
  # test_vrt_check_dont_open_unneeded_source_with_complex_source_nodata
  @parameterized.named_parameters(
      dict(testcase_name='simple_source', source_tag='SimpleSource'),
      dict(testcase_name='complex_source_nodata', source_tag='ComplexSource'),
  )
  def test_dont_open_unneeded_source(self, source_tag: str) -> None:
    vsi_dir = self._make_vsi_dir()
    vrt_path = str(vsi_dir / 'tmp.vrt')
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    nodata_xml = (
        '      <NODATA>0</NODATA>\n' if source_tag == 'ComplexSource' else ''
    )
    gdal.FileFromMemBuffer(
        vrt_path,
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Gray</ColorInterp>\n'
        f'    <{source_tag}>\n'
        f'      <SourceFilename>{byte_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="10" ySize="10" />\n'
        f'{nodata_xml}'
        f'    </{source_tag}>\n'
        f'    <{source_tag}>\n'
        '      <SourceFilename relativeToVRT="1">i_do_not_exist.tif'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="10" yOff="10" xSize="10" ySize="10" />\n'
        '      <DstRect xOff="10" yOff="10" xSize="10" ySize="10" />\n'
        f'{nodata_xml}'
        f'    </{source_tag}>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )
    with gdal.OpenEx(byte_path, gdal.OF_RASTER | gdal.OF_READONLY) as byte_ds:
      expected_arr = byte_ds.GetRasterBand(1).ReadAsArray(0, 0, 10, 10)

    with gdal.Translate(
        '', vrt_path, options='-of MEM -srcwin 0 0 10 10'
    ) as sub_ds:
      np.testing.assert_array_equal(
          sub_ds.GetRasterBand(1).ReadAsArray(), expected_arr
      )

    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, r'No such file or directory'
    ):
      gdal.Translate('', vrt_path, options='-of MEM -srcwin 0 0 10.1 10.1')

  # Upstream: test_vrt_nodata_and_implicit_ovr_recursion_issue
  def test_nodata_and_implicit_ovr_recursion_issue(self) -> None:
    vsi_dir = self._make_vsi_dir()
    vrt_path = str(vsi_dir / 'tmp.vrt')
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    gdal.FileFromMemBuffer(
        vrt_path,
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <NoDataValue>0</NoDataValue>\n'
        '    <ComplexSource>\n'
        '      <NODATA>0</NODATA>\n'
        f'      <SourceFilename>{byte_path}</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '  <OverviewList resampling="average">2</OverviewList>\n'
        '</VRTDataset>',
    )
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      ovr = ds.GetRasterBand(1).GetOverview(0)
      self.assertEqual(ovr.Checksum(), 1152)
      self.assertEqual(ovr.XSize, 10)
      self.assertEqual(ovr.YSize, 10)

  # Upstream: test_vrt_statistics_and_implicit_ovr_recursion_issue
  def test_statistics_and_implicit_ovr_recursion_issue(self) -> None:
    vsi_dir = self._make_vsi_dir()
    scaled_tif = str(vsi_dir / 'test.tif')
    with gdal.Translate(
        scaled_tif, gcore_util.GetTestFilePath('uint16.tif'), width=2048
    ):
      pass

    with gdal.Translate('', scaled_tif, format='VRT') as vrt_ds:
      with gdal.config_option('VRT_VIRTUAL_OVERVIEWS', 'YES'):
        vrt_ds.BuildOverviews('NEAR', [2, 4])

      gdal.ErrorReset()
      stats = vrt_ds.GetRasterBand(1).ComputeStatistics(True)
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      self.assertGreaterEqual(stats[0], 74)
      self.assertLessEqual(stats[0], 90)

      min_max = vrt_ds.GetRasterBand(1).ComputeRasterMinMax(True)
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      self.assertGreaterEqual(min_max[0], 74)
      self.assertLessEqual(min_max[0], 90)

      hist = vrt_ds.GetRasterBand(1).GetHistogram(True)
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      self.assertNotEmpty(hist)

  # Upstream: test_vrt_read_req_coordinates_almost_integer
  def test_req_coordinates_almost_integer(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'in.tif')
    vrt_path = str(vsi_dir / 'in.vrt')
    pattern = bytes(range(255)) * (3601 * 3601 // 255) + b'\x00' * (
        3601 * 3601 % 255
    )
    with self.gtiff_driver.Create(tif_path, 3601, 3601) as ds:
      ds.WriteRaster(0, 0, 3601, 3601, pattern)

    gdal.FileFromMemBuffer(
        vrt_path,
        '<VRTDataset rasterXSize="3601" rasterYSize="3601">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="1">in.tif</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SrcRect xOff="0" yOff="0" xSize="3601" ySize="3601" />\n'
        '      <DstRect xOff="0" yOff="0.499999999992505"'
        ' xSize="1800.50000003" ySize="1800.50000003" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )

    expected_a = np.array(
        [
            [161, 163, 165, 167, 169, 171],
            [223, 225, 227, 229, 231, 233],
            [30, 32, 34, 36, 38, 40],
            [92, 94, 96, 98, 100, 102],
            [154, 156, 158, 160, 162, 164],
            [216, 218, 220, 222, 224, 226],
        ],
        dtype=np.uint8,
    )
    expected_b = np.array(
        [
            [219, 221, 223, 225, 227, 229],
            [26, 28, 30, 32, 34, 36],
            [88, 90, 92, 94, 96, 98],
            [150, 152, 154, 156, 158, 160],
            [212, 214, 216, 218, 220, 222],
            [0, 0, 0, 0, 0, 0],
        ],
        dtype=np.uint8,
    )
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(1045, 1795, 6, 6), expected_a
      )
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(1043, 1796, 6, 6), expected_b
      )

  # Upstream: test_vrt_read_compute_statistics_mosaic_optimization
  @parameterized.named_parameters(
      dict(testcase_name='exact_threaded', approx_ok=False, use_threads=True),
      dict(
          testcase_name='approx_single_thread',
          approx_ok=True,
          use_threads=False,
      ),
  )
  def test_compute_statistics_mosaic_optimization(
      self, approx_ok: bool, use_threads: bool
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    src_filename = str(vsi_dir / 'byte.tif')
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as orig_ds:
      with self.gtiff_driver.CreateCopy(src_filename, orig_ds):
        pass

    with gdal.Translate('', src_filename, format='MEM') as src_ds:
      with gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 0 0 8 20'
      ) as src_ds1, gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 8 0 12 20'
      ) as src_ds2, gdal.BuildVRT(
          '', [src_ds1, src_ds2]
      ) as vrt_ds:
        src_band = src_ds.GetRasterBand(1)
        vrt_band = vrt_ds.GetRasterBand(1)
        with self._vrt_threads_config('2' if use_threads else '0'):
          self.assertEqual(
              vrt_band.ComputeRasterMinMax(approx_ok),
              src_band.ComputeRasterMinMax(approx_ok),
          )
          pcts: list[float] = []

          def callback(
              pct: float, unused_msg: str | None, user_data: list[float]
          ) -> int:
            user_data.append(pct)
            return 1

          vrt_stats = vrt_band.ComputeStatistics(
              approx_ok, callback=callback, callback_data=pcts
          )
        self.assertNotEmpty(pcts)
        self.assertEqual(pcts[-1], 1.0)
        src_stats = src_band.ComputeStatistics(approx_ok)
        for vrt_val, src_val in zip(vrt_stats, src_stats, strict=True):
          self.assertAlmostEqual(vrt_val, src_val)

        if approx_ok:
          self.assertEqual(
              vrt_band.GetMetadataItem('STATISTICS_APPROXIMATE'), 'YES'
          )
        else:
          self.assertIsNone(vrt_band.GetMetadataItem('STATISTICS_APPROXIMATE'))
          self.assertEqual(vrt_band.GetMetadataItem('STATISTICS_MINIMUM'), '74')
          self.assertEqual(
              vrt_band.GetMetadataItem('STATISTICS_MAXIMUM'), '255'
          )
        self.assertEqual(
            vrt_band.GetMetadataItem('STATISTICS_VALID_PERCENT'), '100'
        )

  # Upstream: test_vrt_read_compute_statistics_mosaic_optimization_nodata,
  # test_vrt_read_compute_statistics_mosaic_optimization_nodata_hidden
  @parameterized.named_parameters(
      dict(
          testcase_name='visible_nodata',
          hide_nodata=False,
          expected_valid_pct='90',
      ),
      dict(
          testcase_name='hidden_nodata',
          hide_nodata=True,
          expected_valid_pct='100',
      ),
  )
  def test_compute_statistics_mosaic_optimization_nodata(
      self, hide_nodata: bool, expected_valid_pct: str
  ) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    with gdal.Translate('', byte_path, format='MEM') as src_ds:
      with gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 0 0 8 20'
      ) as src_ds1, gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 10 0 10 20'
      ) as src_ds2, gdal.BuildVRT(
          '', [src_ds1, src_ds2], VRTNodata=10, hideNodata=hide_nodata
      ) as vrt_ds, gdal.Translate(
          '', vrt_ds, format='MEM'
      ) as vrt_mat:
        vrt_band = vrt_ds.GetRasterBand(1)
        mat_band = vrt_mat.GetRasterBand(1)
        with self._vrt_threads_config():
          self.assertEqual(
              vrt_band.ComputeRasterMinMax(False),
              mat_band.ComputeRasterMinMax(False),
          )
          vrt_stats = vrt_band.ComputeStatistics(False)
        mat_stats = mat_band.ComputeStatistics(False)
        for vrt_val, mat_val in zip(vrt_stats, mat_stats, strict=True):
          self.assertAlmostEqual(vrt_val, mat_val)
        self.assertIsNone(vrt_band.GetMetadataItem('STATISTICS_APPROXIMATE'))
        self.assertEqual(
            vrt_band.GetMetadataItem('STATISTICS_VALID_PERCENT'),
            expected_valid_pct,
        )

  # Upstream:
  # test_vrt_read_compute_statistics_mosaic_optimization_src_with_nodata
  @parameterized.named_parameters(
      dict(testcase_name='band_10_global_none', band_ndv=10, global_ndv=None),
      dict(testcase_name='band_1_global_none', band_ndv=1, global_ndv=None),
      dict(testcase_name='band_none_global_10', band_ndv=None, global_ndv=10),
      dict(testcase_name='band_10_global_10', band_ndv=10, global_ndv=10),
  )
  def test_compute_statistics_mosaic_optimization_src_with_nodata(
      self, band_ndv: int | None, global_ndv: int | None
  ) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    with gdal.Translate('', byte_path, format='MEM') as src_ds:
      with gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 0 0 8 20 -scale 0 255 10 10'
      ) as src_ds1, gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 8 0 12 20'
      ) as src_ds2, gdal.BuildVRT(
          '', [src_ds1, src_ds2]
      ) as vrt_ds:
        if band_ndv is not None:
          src_ds1.GetRasterBand(1).SetNoDataValue(band_ndv)
        if global_ndv is not None:
          vrt_ds.GetRasterBand(1).SetNoDataValue(global_ndv)
        with gdal.Translate('', vrt_ds, format='MEM') as vrt_mat:
          vrt_band = vrt_ds.GetRasterBand(1)
          mat_band = vrt_mat.GetRasterBand(1)
          with self._vrt_threads_config():
            self.assertEqual(
                vrt_band.ComputeRasterMinMax(False),
                mat_band.ComputeRasterMinMax(False),
            )
            vrt_stats = vrt_band.ComputeStatistics(False)
          mat_stats = mat_band.ComputeStatistics(False)
          for vrt_val, mat_val in zip(vrt_stats, mat_stats, strict=True):
            self.assertAlmostEqual(vrt_val, mat_val)

  # Upstream:
  # test_vrt_read_compute_statistics_mosaic_optimization_src_with_nodata_all
  def test_compute_statistics_mosaic_optimization_all_nodata(self) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    with gdal.Translate('', byte_path, format='MEM') as src_ds:
      with gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 0 0 8 20 -scale 0 255 10 10'
      ) as src_ds1, gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 8 0 12 20 -scale 0 255 10 10'
      ) as src_ds2, gdal.BuildVRT(
          '', [src_ds1, src_ds2], VRTNodata=10
      ) as vrt_ds:
        src_ds1.GetRasterBand(1).SetNoDataValue(10)
        src_ds2.GetRasterBand(1).SetNoDataValue(10)
        vrt_band = vrt_ds.GetRasterBand(1)
        with self._vrt_threads_config():
          with gdal.quiet_errors(), self.assertRaisesRegex(
              RuntimeError, r'no valid pixels'
          ):
            vrt_band.ComputeRasterMinMax(False)

          with gdal.quiet_errors(), self.assertRaisesRegex(
              RuntimeError, r'no valid pixels'
          ):
            vrt_band.ComputeStatistics(False)
        self.assertIsNone(vrt_band.GetMetadataItem('STATISTICS_MINIMUM'))

  # Upstream:
  # test_vrt_read_compute_statistics_mosaic_optimization_not_triggered
  def test_compute_statistics_mosaic_optimization_overlapping_sources(
      self,
  ) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    with gdal.Translate('', byte_path, format='MEM') as src_ds:
      with gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 0 0 10 20'
      ) as src_ds1, gdal.Translate(
          '', src_ds, options='-of MEM -srcwin 9 0 11 20'
      ) as src_ds2, gdal.BuildVRT(
          '', [src_ds1, src_ds2]
      ) as vrt_ds:
        vrt_band = vrt_ds.GetRasterBand(1)
        src_band = src_ds.GetRasterBand(1)
        self.assertEqual(
            vrt_band.ComputeRasterMinMax(), src_band.ComputeRasterMinMax()
        )
        vrt_stats = vrt_band.ComputeStatistics(False)
        src_stats = src_band.ComputeStatistics(False)
        for vrt_val, src_val in zip(vrt_stats, src_stats, strict=True):
          self.assertAlmostEqual(vrt_val, src_val)

  # Upstream:
  # test_vrt_read_compute_statistics_mosaic_optimization_single_source
  def test_compute_statistics_single_source_preserves_metadata(self) -> None:
    vsi_dir = self._make_vsi_dir()
    in_tif = str(vsi_dir / 'in.tif')
    test_tif = str(vsi_dir / 'test.tif')
    test_vrt = str(vsi_dir / 'test.vrt')
    rng = np.random.default_rng(0)
    side = 512

    with self.gtiff_driver.Create(
        in_tif, side, side, 1, gdal.GDT_Float32, options=['TILED=YES']
    ) as ds:
      ds.SetGeoTransform([0.0, 1.0, 0.0, 0.0, 0.0, -1.0])
      ds.WriteArray(rng.standard_normal(side * side).reshape(side, side))

    with gdal.Translate(test_tif, in_tif, options='-stats -co TILED=YES'):
      pass
    with gdal.OpenEx(test_tif, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      md_band_1 = ds.GetRasterBand(1).GetMetadata()

    with gdal.BuildVRT(test_vrt, test_tif):
      pass
    with gdal.OpenEx(test_vrt, gdal.OF_RASTER | gdal.OF_UPDATE) as ds:
      ds.GetRasterBand(1).ComputeStatistics(False)
    with gdal.OpenEx(test_vrt, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetMetadata(), md_band_1)
    with gdal.OpenEx(test_tif, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetMetadata(), md_band_1)

  # Upstream: test_vrt_read_complex_source_use_band_data_type_constraint
  @parameterized.named_parameters(
      dict(
          testcase_name='ds_byte',
          use_dataset=True,
          struct_char='B',
          buf_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='ds_int32',
          use_dataset=True,
          struct_char='i',
          buf_type=gdal.GDT_Int32,
      ),
      dict(
          testcase_name='band_byte',
          use_dataset=False,
          struct_char='B',
          buf_type=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='band_int32',
          use_dataset=False,
          struct_char='i',
          buf_type=gdal.GDT_Int32,
      ),
  )
  def test_complex_source_band_data_type_clamping(
      self, use_dataset: bool, struct_char: str, buf_type: int
  ) -> None:
    byte_path = gcore_util.GetTestFilePath('byte.tif')
    vrt_xml = (
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename relativeToVRT="0">{byte_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <ScaleOffset>0</ScaleOffset>\n'
        '      <ScaleRatio>1.5</ScaleRatio>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      reader = ds if use_dataset else ds.GetRasterBand(1)
      scaled_data = struct.unpack(
          struct_char * (20 * 20), reader.ReadRaster(buf_type=buf_type)
      )
    self.assertEqual(max(scaled_data), 255)

  # Upstream: test_vrt_read_top_and_bottom_strips_average
  def test_top_and_bottom_strips_average(self) -> None:
    vsi_dir = self._make_vsi_dir()
    top_tif = str(vsi_dir / 'top_strip.tif')
    bottom_tif = str(vsi_dir / 'bottom_strip.tif')
    vrt_path = str(vsi_dir / 'strips.vrt')

    with self.gtiff_driver.Create(top_tif, 256, 128, 1, gdal.GDT_Byte) as ds:
      ds.GetRasterBand(1).Fill(0)
    with self.gtiff_driver.Create(bottom_tif, 256, 128, 1, gdal.GDT_Byte) as ds:
      ds.GetRasterBand(1).Fill(255)

    gdal.FileFromMemBuffer(
        vrt_path,
        '<VRTDataset rasterXSize="256" rasterYSize="256">\n'
        '  <GeoTransform>0, 1, 0, 0, 0, -1</GeoTransform>\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Gray</ColorInterp>\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="1">top_strip.tif'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="256" RasterYSize="128"'
        ' DataType="Byte" BlockXSize="256" BlockYSize="128" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="256" ySize="128" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="256" ySize="128" />\n'
        '    </SimpleSource>\n'
        '    <SimpleSource>\n'
        '      <SourceFilename relativeToVRT="1">bottom_strip.tif'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="256" RasterYSize="128"'
        ' DataType="Byte" BlockXSize="256" BlockYSize="128" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="256" ySize="128" />\n'
        '      <DstRect xOff="0" yOff="128" xSize="256" ySize="128" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )

    kwargs = {
        'buf_xsize': 128,
        'buf_ysize': 64,
        'resample_alg': gdal.GRIORA_Average,
    }
    with gdal.Translate('', vrt_path, format='MEM') as ref_ds:
      ref_data = ref_ds.ReadRaster(0, 127, 256, 129, **kwargs)
    with gdal.OpenEx(vrt_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          ds.GetRasterBand(1).ReadRaster(0, 127, 256, 129, **kwargs), ref_data
      )
      self.assertEqual(ds.ReadRaster(0, 127, 256, 129, **kwargs), ref_data)
    self.assertEqual(ref_data[0], 128)

  # Upstream: test_vrt_read_complex_source_nodata
  @parameterized.named_parameters(
      dict(
          testcase_name='byte_to_byte_nd0',
          input_dt=gdal.GDT_Byte,
          vrt_type='Byte',
          nodata=0,
          vrt_nodata=255,
          request_dt=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_to_byte_nd254',
          input_dt=gdal.GDT_Byte,
          vrt_type='Byte',
          nodata=254,
          vrt_nodata=255,
          request_dt=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_to_int8_req_byte',
          input_dt=gdal.GDT_Byte,
          vrt_type='Int8',
          nodata=254,
          vrt_nodata=255,
          request_dt=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_to_byte_req_int8',
          input_dt=gdal.GDT_Byte,
          vrt_type='Byte',
          nodata=254,
          vrt_nodata=127,
          request_dt=gdal.GDT_Int8,
      ),
      dict(
          testcase_name='byte_to_uint16_req_byte',
          input_dt=gdal.GDT_Byte,
          vrt_type='UInt16',
          nodata=254,
          vrt_nodata=255,
          request_dt=gdal.GDT_Byte,
      ),
      dict(
          testcase_name='byte_to_byte_req_uint16',
          input_dt=gdal.GDT_Byte,
          vrt_type='Byte',
          nodata=254,
          vrt_nodata=255,
          request_dt=gdal.GDT_UInt16,
      ),
      dict(
          testcase_name='int8_to_int8',
          input_dt=gdal.GDT_Int8,
          vrt_type='Int8',
          nodata=0,
          vrt_nodata=127,
          request_dt=gdal.GDT_Int8,
      ),
      dict(
          testcase_name='int8_to_int16_req_int8',
          input_dt=gdal.GDT_Int8,
          vrt_type='Int16',
          nodata=0,
          vrt_nodata=127,
          request_dt=gdal.GDT_Int8,
      ),
      dict(
          testcase_name='uint16_to_uint16',
          input_dt=gdal.GDT_UInt16,
          vrt_type='UInt16',
          nodata=0,
          vrt_nodata=65535,
          request_dt=gdal.GDT_UInt16,
      ),
      dict(
          testcase_name='int16_to_int16',
          input_dt=gdal.GDT_Int16,
          vrt_type='Int16',
          nodata=0,
          vrt_nodata=32767,
          request_dt=gdal.GDT_Int16,
      ),
      dict(
          testcase_name='uint32_to_uint32',
          input_dt=gdal.GDT_UInt32,
          vrt_type='UInt32',
          nodata=0,
          vrt_nodata=(1 << 31) - 1,
          request_dt=gdal.GDT_UInt32,
      ),
      dict(
          testcase_name='int32_to_int32',
          input_dt=gdal.GDT_Int32,
          vrt_type='Int32',
          nodata=0,
          vrt_nodata=(1 << 30) - 1,
          request_dt=gdal.GDT_Int32,
      ),
      dict(
          testcase_name='int32_to_float32_req_float64',
          input_dt=gdal.GDT_Int32,
          vrt_type='Float32',
          nodata=0,
          vrt_nodata=(1 << 30) - 1,
          request_dt=gdal.GDT_Float64,
      ),
      dict(
          testcase_name='uint64_to_uint64',
          input_dt=gdal.GDT_UInt64,
          vrt_type='UInt64',
          nodata=0,
          vrt_nodata=(1 << 63) - 1,
          request_dt=gdal.GDT_UInt64,
      ),
      dict(
          testcase_name='int64_to_int64',
          input_dt=gdal.GDT_Int64,
          vrt_type='Int64',
          nodata=0,
          vrt_nodata=(1 << 62) - 1,
          request_dt=gdal.GDT_Int64,
      ),
      dict(
          testcase_name='int64_to_float32_req_int64',
          input_dt=gdal.GDT_Int64,
          vrt_type='Float32',
          nodata=0,
          vrt_nodata=(1 << 62),
          request_dt=gdal.GDT_Int64,
      ),
      dict(
          testcase_name='float32_to_float32',
          input_dt=gdal.GDT_Float32,
          vrt_type='Float32',
          nodata=0,
          vrt_nodata=1.5,
          request_dt=gdal.GDT_Float32,
      ),
      dict(
          testcase_name='float32_to_float32_req_float64',
          input_dt=gdal.GDT_Float32,
          vrt_type='Float32',
          nodata=0,
          vrt_nodata=1.5,
          request_dt=gdal.GDT_Float64,
      ),
      dict(
          testcase_name='float32_to_float64_req_float32',
          input_dt=gdal.GDT_Float32,
          vrt_type='Float64',
          nodata=0,
          vrt_nodata=1.5,
          request_dt=gdal.GDT_Float32,
      ),
      dict(
          testcase_name='float32_to_float64_req_float64',
          input_dt=gdal.GDT_Float32,
          vrt_type='Float64',
          nodata=0,
          vrt_nodata=1.5,
          request_dt=gdal.GDT_Float64,
      ),
      dict(
          testcase_name='float64_to_float64',
          input_dt=gdal.GDT_Float64,
          vrt_type='Float64',
          nodata=0,
          vrt_nodata=1.5,
          request_dt=gdal.GDT_Float64,
      ),
      dict(
          testcase_name='float64_to_float32_req_float64',
          input_dt=gdal.GDT_Float64,
          vrt_type='Float32',
          nodata=0,
          vrt_nodata=1.5,
          request_dt=gdal.GDT_Float64,
      ),
  )
  def test_complex_source_nodata(
      self,
      input_dt: int,
      vrt_type: str,
      nodata: float,
      vrt_nodata: float,
      request_dt: int,
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    input_filename = str(vsi_dir / 'source.tif')
    if input_dt in (gdal.GDT_Float32, gdal.GDT_Float64):
      input_val: float = 1.75
      if vrt_type in ('Float32', 'Float64') and request_dt in (
          gdal.GDT_Float32,
          gdal.GDT_Float64,
      ):
        expected_val: float = input_val
      else:
        expected_val = round(input_val)
    else:
      input_val = 1
      expected_val = input_val

    input_data = array.array(
        _DATA_TYPE_TO_STRUCT_CHAR[input_dt],
        [
            nodata,
            input_val,
            2,
            3,
            nodata,
            4,
            nodata,
            5,
            6,
            7,
            16,
            17,
            18,
            19,
            20,
            8,
            9,
            nodata,
            10,
            11,
            nodata,
            nodata,
            nodata,
            nodata,
            nodata,
            12,
            13,
            14,
            nodata,
            15,
        ],
    )
    with self.gtiff_driver.Create(input_filename, 5, 6, 1, input_dt) as ds:
      ds.WriteRaster(0, 0, 5, 6, input_data.tobytes(), buf_type=input_dt)

    vrt_xml = (
        '<VRTDataset rasterXSize="5" rasterYSize="6">\n'
        f'  <VRTRasterBand dataType="{vrt_type}" band="1">\n'
        f'    <NoDataValue>{vrt_nodata}</NoDataValue>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename relativeToVRT="0">{input_filename}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        f'      <NODATA>{nodata}</NODATA>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      got_data = struct.unpack(
          _DATA_TYPE_TO_STRUCT_CHAR[request_dt] * (5 * 6),
          vrt_ds.ReadRaster(buf_type=request_dt),
      )

    expected = (
        vrt_nodata,
        expected_val,
        2,
        3,
        vrt_nodata,
        4,
        vrt_nodata,
        5,
        6,
        7,
        16,
        17,
        18,
        19,
        20,
        8,
        9,
        vrt_nodata,
        10,
        11,
        vrt_nodata,
        vrt_nodata,
        vrt_nodata,
        vrt_nodata,
        vrt_nodata,
        12,
        13,
        14,
        vrt_nodata,
        15,
    )
    self.assertEqual(got_data, expected)

  # Upstream: test_vrt_read_complex_source_nodata_out_of_range
  @parameterized.named_parameters(
      dict(testcase_name='byte', data_type=gdal.GDT_Byte),
      dict(testcase_name='uint16', data_type=gdal.GDT_UInt16),
      dict(testcase_name='int16', data_type=gdal.GDT_Int16),
  )
  def test_complex_source_nodata_out_of_range(self, data_type: int) -> None:
    vsi_dir = self._make_vsi_dir()
    input_filename = str(vsi_dir / 'source.tif')
    array_type = _DATA_TYPE_TO_STRUCT_CHAR[data_type]
    with self.gtiff_driver.Create(input_filename, 1, 1, 1, data_type) as ds:
      ds.WriteRaster(
          0, 0, 1, 1, array.array(array_type, [1]).tobytes(), buf_type=data_type
      )

    vrt_type = gdal.GetDataTypeName(data_type)
    vrt_xml = (
        '<VRTDataset rasterXSize="1" rasterYSize="1">\n'
        f'  <VRTRasterBand dataType="{vrt_type}" band="1">\n'
        '    <NoDataValue>255</NoDataValue>\n'
        '    <ComplexSource>\n'
        f'      <SourceFilename relativeToVRT="0">{input_filename}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <NODATA>-100000000</NODATA>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_RASTER | gdal.OF_READONLY) as vrt_ds:
      got_data = struct.unpack(
          array_type, vrt_ds.ReadRaster(buf_type=data_type)
      )
    self.assertEqual(got_data, (1,))

  # Upstream: test_vrt_read_compute_statistics_approximate
  def test_compute_statistics_approximate_with_overview(self) -> None:
    vsi_dir = self._make_vsi_dir()
    gtiff_filename = str(vsi_dir / 'tmp.tif')
    vrt_filename = str(vsi_dir / 'tmp.vrt')
    with gdal.Translate(
        gtiff_filename,
        gcore_util.GetTestFilePath('byte.tif'),
        format='GTiff',
        width=256,
    ):
      pass
    with gdal.Translate(vrt_filename, gtiff_filename):
      pass

    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      ds.BuildOverviews('NEAR', [2])
    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      ds.GetRasterBand(1).ComputeStatistics(True)
    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      md = ds.GetRasterBand(1).GetMetadata()
    self.assertEqual(md['STATISTICS_APPROXIMATE'], 'YES')
    self.assertIn('STATISTICS_MINIMUM', md)
    self.assertIn('STATISTICS_MAXIMUM', md)
    self.assertIn('STATISTICS_MEAN', md)
    self.assertIn('STATISTICS_STDDEV', md)
    self.assertEqual(md['STATISTICS_VALID_PERCENT'], '100')

  # Upstream: test_vrt_read_virtual_overviews_match_src_overviews
  def test_virtual_overviews_match_src_overviews(self) -> None:
    vsi_dir = self._make_vsi_dir()
    gtiff_filename = str(vsi_dir / 'tmp.tif')
    with self.gtiff_driver.Create(
        gtiff_filename, 4095, 2047, options=['SPARSE_OK=YES']
    ) as ds:
      ds.BuildOverviews('NONE', [2, 4, 8])
      band = ds.GetRasterBand(1)
      # Precondition: GTiff overview dimensions match expected rounding.
      self.assertEqual(band.GetOverview(0).XSize, 2048)
      self.assertEqual(band.GetOverview(0).YSize, 1024)
      self.assertEqual(band.GetOverview(1).XSize, 1024)
      self.assertEqual(band.GetOverview(1).YSize, 512)

    with gdal.OpenEx(
        f'vrt://{gtiff_filename}?outsize=200%,200%',
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as vrt_ds:
      self.assertEqual(vrt_ds.RasterXSize, 4095 * 2)
      self.assertEqual(vrt_ds.RasterYSize, 2047 * 2)
      vrt_band = vrt_ds.GetRasterBand(1)
      self.assertEqual(vrt_band.XSize, 4095 * 2)
      self.assertEqual(vrt_band.YSize, 2047 * 2)
      self.assertEqual(vrt_band.GetOverviewCount(), 3)
      self.assertEqual(vrt_band.GetOverview(0).XSize, 4095)
      self.assertEqual(vrt_band.GetOverview(0).YSize, 2047)
      self.assertEqual(vrt_band.GetOverview(1).XSize, 2048)
      self.assertEqual(vrt_band.GetOverview(1).YSize, 1024)
      self.assertEqual(vrt_band.GetOverview(2).XSize, 1024)
      self.assertEqual(vrt_band.GetOverview(2).YSize, 512)

  # Upstream: test_vrt_read_multi_threaded
  @parameterized.named_parameters(
      dict(
          testcase_name='ds_threaded_2tiles',
          dataset_level=True,
          use_threads=True,
          num_tiles=2,
      ),
      dict(
          testcase_name='ds_threaded_128tiles',
          dataset_level=True,
          use_threads=True,
          num_tiles=128,
      ),
      dict(
          testcase_name='ds_single_2tiles',
          dataset_level=True,
          use_threads=False,
          num_tiles=2,
      ),
      dict(
          testcase_name='ds_single_128tiles',
          dataset_level=True,
          use_threads=False,
          num_tiles=128,
      ),
      dict(
          testcase_name='band_threaded_2tiles',
          dataset_level=False,
          use_threads=True,
          num_tiles=2,
      ),
      dict(
          testcase_name='band_threaded_128tiles',
          dataset_level=False,
          use_threads=True,
          num_tiles=128,
      ),
      dict(
          testcase_name='band_single_2tiles',
          dataset_level=False,
          use_threads=False,
          num_tiles=2,
      ),
      dict(
          testcase_name='band_single_128tiles',
          dataset_level=False,
          use_threads=False,
          num_tiles=128,
      ),
  )
  def test_multi_threaded_read(
      self, dataset_level: bool, use_threads: bool, num_tiles: int
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    width = 2048
    small_world_path = gcore_util.GetTestFilePath('small_world.tif')
    tile_width = width // num_tiles
    tile_filenames = []

    with gdal.Translate(
        '', small_world_path, width=width, format='MEM'
    ) as src_ds:
      for i in range(num_tiles):
        tile_filename = str(vsi_dir / f'{i}.tif')
        with gdal.Translate(
            tile_filename,
            src_ds,
            srcWin=[i * tile_width, 0, tile_width, 1024],
        ):
          pass
        tile_filenames.append(tile_filename)

      vrt_filename = str(vsi_dir / 'test.vrt')
      with gdal.BuildVRT(vrt_filename, tile_filenames):
        pass

      with gdal.OpenEx(
          vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY
      ) as vrt_ds:
        reader = vrt_ds if dataset_level else vrt_ds.GetRasterBand(1)
        ref_reader = src_ds if dataset_level else src_ds.GetRasterBand(1)
        expected_bytes = ref_reader.ReadRaster(1, 2, 1030, 1020)

        pcts: list[float] = []

        def cbk(pct: float, unused_msg: str | None, unused_data: None) -> int:
          pcts.append(pct)
          return 1

        with self._vrt_threads_config(None if use_threads else '0'):
          got_bytes = reader.ReadRaster(1, 2, 1030, 1020, callback=cbk)

        self.assertEqual(got_bytes, expected_bytes)
        self.assertNotEmpty(pcts)
        self.assertEqual(pcts[-1], 1.0)
        self.assertTrue(all(a <= b for a, b in zip(pcts, pcts[1:])))
        expected_threaded = (
            '1'
            if gdal.GetNumCPUs() >= 2 and use_threads and not _is_tsan()
            else '0'
        )
        self.assertEqual(
            vrt_ds.GetMetadataItem(
                'MULTI_THREADED_RASTERIO_LAST_USED', '__DEBUG__'
            ),
            expected_threaded,
        )

  # Upstream: test_vrt_read_multi_threaded_disabled_since_overlapping_sources
  def test_multi_threaded_disabled_for_overlapping_sources(self) -> None:
    small_world_path = gcore_util.GetTestFilePath('small_world.tif')
    with gdal.Translate('', small_world_path, width=2048, format='MEM') as src:
      with gdal.Translate(
          'left', src, format='MEM', srcWin=[0, 0, 1025, 1024]
      ) as left_ds, gdal.Translate(
          'right', src, format='MEM', srcWin=[1024, 0, 1024, 1024]
      ) as right_ds, gdal.BuildVRT(
          '', [left_ds, right_ds]
      ) as vrt_ds:
        self.assertEqual(
            vrt_ds.ReadRaster(1, 2, 1030, 1020),
            src.ReadRaster(1, 2, 1030, 1020),
        )
        self.assertEqual(
            vrt_ds.GetMetadataItem(
                'MULTI_THREADED_RASTERIO_LAST_USED', '__DEBUG__'
            ),
            '0',
        )

  # Upstream: test_vrt_read_multi_threaded_errors
  def test_multi_threaded_errors_propagate(self) -> None:
    vsi_dir = self._make_vsi_dir()
    filename1 = str(vsi_dir / 'tmp1.tif')
    filename2 = str(vsi_dir / 'tmp2.tif')
    vrt_filename = str(vsi_dir / 'tmp.vrt')

    with self.gtiff_driver.Create(filename1, 1, 1) as ds:
      ds.SetGeoTransform([2.0, 1.0, 0.0, 49.0, 0.0, -1.0])
    with self.gtiff_driver.Create(filename2, 1, 1) as ds:
      ds.SetGeoTransform([3.0, 1.0, 0.0, 49.0, 0.0, -1.0])
    with gdal.BuildVRT(vrt_filename, [filename1, filename2]):
      pass
    gdal.Unlink(filename2)

    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with (
          self._vrt_threads_config(),
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'No such file or directory'),
      ):
        ds.GetRasterBand(1).ReadAsArray()

  # Upstream: test_vrt_read_nested_VRTDataset, test_vrt_update_nested_VRTDataset
  def test_nested_vrt_dataset_read_and_stats_update(self) -> None:
    vsi_dir = self._make_vsi_dir()
    with gdal.OpenEx(
        gcore_util.GetTestFilePath('byte.tif'),
        gdal.OF_RASTER | gdal.OF_READONLY,
    ) as src_ds:
      expected_arr = src_ds.GetRasterBand(1).ReadAsArray()
      with self.gtiff_driver.CreateCopy(str(vsi_dir / 'byte.tif'), src_ds):
        pass

    nested_dir = vsi_dir / 'vrt'
    gdal.Mkdir(str(nested_dir), 0o755)
    vrt_filename = str(nested_dir / 'nested_VRTDataset.vrt')
    gdal.FileFromMemBuffer(
        vrt_filename,
        '<VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '  <VRTRasterBand dataType="Byte" band="1">\n'
        '    <ColorInterp>Gray</ColorInterp>\n'
        '    <SimpleSource>\n'
        '      <VRTDataset rasterXSize="20" rasterYSize="20">\n'
        '        <VRTRasterBand dataType="Byte" band="1">\n'
        '          <SimpleSource>\n'
        '            <SourceFilename relativeToVRT="1">../byte.tif'
        '</SourceFilename>\n'
        '            <SourceBand>1</SourceBand>\n'
        '            <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '            <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '            <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '          </SimpleSource>\n'
        '        </VRTRasterBand>\n'
        '      </VRTDataset>\n'
        '      <SourceBand>1</SourceBand>\n'
        '      <SourceProperties RasterXSize="20" RasterYSize="20"'
        ' DataType="Byte" BlockXSize="20" BlockYSize="20" />\n'
        '      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '      <DstRect xOff="0" yOff="0" xSize="20" ySize="20" />\n'
        '    </SimpleSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>',
    )

    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 4672)
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)
      self.assertIsNone(band.GetMinimum())
      vrt_stats = band.ComputeStatistics(False)
      self.assertEqual(vrt_stats[0], 74.0)

    # Reopen to verify computed statistics were serialized back into the VRT.
    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 4672)
      self.assertEqual(band.GetMinimum(), 74.0)

  # Upstream: test_vrt_read_cfloat32_complex_source_as_float32,
  # test_vrt_read_float32_complex_source_from_cfloat32
  def test_cfloat32_and_float32_complex_source_conversions(self) -> None:
    vsi_dir = self._make_vsi_dir()
    tif_path = str(vsi_dir / 'cfloat32.tif')
    with self.gtiff_driver.Create(tif_path, 2, 2, 1, gdal.GDT_CFloat32) as ds:
      ds.GetRasterBand(1).WriteArray(
          np.full((2, 2), 1.0 + 0.0j, dtype=np.complex64)
      )

    cfloat_vrt = (
        '<VRTDataset rasterXSize="2" rasterYSize="2">\n'
        '  <VRTRasterBand dataType="CFloat32" band="1">\n'
        '    <ComplexSource>\n'
        '      <NODATA>0</NODATA>\n'
        f'      <SourceFilename relativeToVRT="0">{tif_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(cfloat_vrt, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          struct.unpack('f' * 8, ds.ReadRaster()),
          (1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0),
      )
      self.assertEqual(
          struct.unpack('f' * 4, ds.ReadRaster(buf_type=gdal.GDT_Float32)),
          (1.0, 1.0, 1.0, 1.0),
      )

    float_vrt = (
        '<VRTDataset rasterXSize="2" rasterYSize="2">\n'
        '  <VRTRasterBand dataType="Float32" band="1">\n'
        '    <ComplexSource>\n'
        '      <NODATA>0</NODATA>\n'
        f'      <SourceFilename relativeToVRT="0">{tif_path}'
        '</SourceFilename>\n'
        '      <SourceBand>1</SourceBand>\n'
        '    </ComplexSource>\n'
        '  </VRTRasterBand>\n'
        '</VRTDataset>'
    )
    with gdal.OpenEx(float_vrt, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          struct.unpack('f' * 4, ds.ReadRaster()), (1.0, 1.0, 1.0, 1.0)
      )

  # Upstream: test_vrt_resampling_with_mask_and_overviews
  @parameterized.named_parameters(
      dict(
          testcase_name='cubic',
          resample_alg=gdal.GRIORA_Cubic,
          expected_pixels=b'\xff\xff\xff\xff\xfc\xf0\xf0\xf0\xf0\xf0',
      ),
      dict(
          testcase_name='bilinear',
          resample_alg=gdal.GRIORA_Bilinear,
          expected_pixels=b'\xff\xff\xff\xff\xfb\xf1\xf0\xf0\xf0\xf0',
      ),
  )
  def test_resampling_with_mask_and_overviews(
      self, resample_alg: int, expected_pixels: bytes
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    filename1 = str(vsi_dir / 'in1.tif')
    filename2 = str(vsi_dir / 'in2.tif')
    vrt_filename = str(vsi_dir / 'test.vrt')

    with self.gtiff_driver.Create(filename1, 100, 10) as ds:
      ds.SetGeoTransform([0.0, 1.0, 0.0, 0.0, 0.0, -1.0])
      ds.GetRasterBand(1).WriteRaster(0, 0, 48, 10, b'\xff' * (48 * 10))
      ds.GetRasterBand(1).WriteRaster(48, 0, 2, 10, b'\xf0' * (2 * 10))
      ds.CreateMaskBand(gdal.GMF_PER_DATASET)
      ds.GetRasterBand(1).GetMaskBand().WriteRaster(
          0, 0, 52, 10, b'\xff' * (52 * 10)
      )
      ds.BuildOverviews('NEAR', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(127)

    with self.gtiff_driver.Create(filename2, 100, 10) as ds:
      ds.SetGeoTransform([0.0, 1.0, 0.0, 0.0, 0.0, -1.0])
      ds.GetRasterBand(1).WriteRaster(48, 0, 52, 10, b'\xf0' * (52 * 10))
      ds.CreateMaskBand(gdal.GMF_PER_DATASET)
      ds.GetRasterBand(1).GetMaskBand().WriteRaster(
          48, 0, 52, 10, b'\xff' * (52 * 10)
      )
      ds.BuildOverviews('NEAR', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(127)

    with gdal.BuildVRT(
        vrt_filename, [filename1, filename2], resampleAlg=resample_alg
    ):
      pass
    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          ds.ReadRaster(buf_xsize=10, buf_ysize=1), expected_pixels
      )
      mask_band = ds.GetRasterBand(1).GetMaskBand()
      self.assertEqual(
          mask_band.ReadRaster(buf_xsize=10, buf_ysize=1), b'\xff' * 10
      )

  # Upstream: test_vrt_resampling_with_alpha_and_overviews
  @parameterized.named_parameters(
      dict(
          testcase_name='cubic',
          resample_alg=gdal.GRIORA_Cubic,
          expected_pixels=b'\xff\xff\xff\xff\xfc\xf0\xf0\xf0\xf0\xf0',
      ),
      dict(
          testcase_name='bilinear',
          resample_alg=gdal.GRIORA_Bilinear,
          expected_pixels=b'\xff\xff\xff\xff\xfb\xf1\xf0\xf0\xf0\xf0',
      ),
  )
  def test_resampling_with_alpha_and_overviews(
      self, resample_alg: int, expected_pixels: bytes
  ) -> None:
    vsi_dir = self._make_vsi_dir()
    filename1 = str(vsi_dir / 'in1.tif')
    filename2 = str(vsi_dir / 'in2.tif')
    vrt_filename = str(vsi_dir / 'test.vrt')

    with self.gtiff_driver.Create(
        filename1, 100, 10, 2, options=['ALPHA=YES']
    ) as ds:
      ds.SetGeoTransform([0.0, 1.0, 0.0, 0.0, 0.0, -1.0])
      ds.GetRasterBand(1).WriteRaster(0, 0, 48, 10, b'\xff' * (48 * 10))
      ds.GetRasterBand(1).WriteRaster(48, 0, 2, 10, b'\xf0' * (2 * 10))
      ds.GetRasterBand(2).WriteRaster(0, 0, 52, 10, b'\xff' * (52 * 10))
      ds.BuildOverviews('NEAR', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(127)

    with self.gtiff_driver.Create(
        filename2, 100, 10, 2, options=['ALPHA=YES']
    ) as ds:
      ds.SetGeoTransform([0.0, 1.0, 0.0, 0.0, 0.0, -1.0])
      ds.GetRasterBand(1).WriteRaster(48, 0, 52, 10, b'\xf0' * (52 * 10))
      ds.GetRasterBand(2).WriteRaster(48, 0, 52, 10, b'\xff' * (52 * 10))
      ds.BuildOverviews('NEAR', [2])
      ds.GetRasterBand(1).GetOverview(0).Fill(127)

    with gdal.BuildVRT(
        vrt_filename, [filename1, filename2], resampleAlg=resample_alg
    ):
      pass
    with gdal.OpenEx(vrt_filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(
          ds.ReadRaster(buf_xsize=10, buf_ysize=1),
          expected_pixels + (b'\xff' * 10),
      )

  # Upstream: test_vrt_read_CheckCompatibleForDatasetIO
  def test_check_compatible_for_dataset_io(self) -> None:
    rgb_path = gcore_util.GetTestFilePath('rgbsmall.tif')
    with gdal.Translate('', rgb_path, format='MEM') as mem_ds:
      with gdal.Translate('', mem_ds, width=25, format='VRT') as vrt_ds:
        self.assertEqual(
            vrt_ds.GetMetadataItem(
                'CheckCompatibleForDatasetIO()', '__DEBUG__'
            ),
            '1',
        )


if __name__ == '__main__':
  if len(sys.argv) >= 3 and sys.argv[1] == _SMALL_POOL_CHILD_FLAG:
    googletest.ThisTestIsUsefulWithoutCallingMain()
    _verify_two_level_vrt_small_pool(pathlib.Path(sys.argv[2]))
  else:
    googletest.main()
