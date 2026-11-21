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
# Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT

"""Tests GeoTIFF reading in GDAL.

Format is described here:
  https://gdal.org/en/latest/drivers/raster/gtiff.html

Rewrite of:
  https://github.com/OSGeo/gdal/blob/master/autotest/gcore/tiff_read.py
"""

from collections.abc import Sequence
import json
import os
import pathlib
import shutil
import struct
from unittest import mock

import numpy as np
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.GTIFF_DRIVER
EXT = '.tif'

# Unit default transform:
# 0 - Top left x: 0
# 1 - E-W resolution: 1
# 2 - No rotation
# 3 - Top left y: 0
# 4 - No rotation
# 5 - N-S resolution: 1
EMPTY_GEOTRANSFORM = (0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
BYTE_GEOTRANSFORM = (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0)

# TIFF field type codes.
_TIFF_BYTE = 1
_TIFF_ASCII = 2
_TIFF_SHORT = 3
_TIFF_LONG = 4
_TIFF_RATIONAL = 5
_TIFF_DOUBLE = 12
_TIFF_LONG8 = 16

_TIFF_PACK_FORMATS = {
    _TIFF_BYTE: ('B', 1),
    _TIFF_SHORT: ('H', 2),
    _TIFF_LONG: ('I', 4),
    _TIFF_LONG8: ('Q', 8),
}


class _TailStripOffsets:
  """Placeholder replaced with sequential offsets into tail_bytes."""

  def __init__(
      self,
      count: int = 1,
      field_type: int = _TIFF_LONG,
      step: int = 1,
  ) -> None:
    if field_type not in _TIFF_PACK_FORMATS:
      raise ValueError(f'Unsupported strile offset field type: {field_type}')
    self.count = count
    self.field_type = field_type
    self.step = step


def _pack_strile_offsets(
    spec: _TailStripOffsets, tail_offset: int, endian: str
) -> bytes:
  fmt_char, _ = _TIFF_PACK_FORMATS[spec.field_type]
  vals = [tail_offset + i * spec.step for i in range(spec.count)]
  if spec.field_type == _TIFF_BYTE:
    return struct.pack(f'{spec.count}B', *vals)
  return struct.pack(f'{endian}{spec.count}{fmt_char}', *vals)


def _align_even(buf: bytearray) -> None:
  """Pads a byte buffer with NUL to a 2-byte TIFF word boundary."""
  if len(buf) % 2:
    buf.append(0)


def _pack_inline_or_offset(
    value: int | bytes | _TailStripOffsets,
    field_type: int,
    endian: str,
    inline_size: int,
    payloads: bytearray,
    payload_base: int,
    tail_slots: list[tuple[bool, int, _TailStripOffsets]] | None = None,
    entry_val_pos: int = 0,
) -> bytes:
  """Encodes a TIFF tag ValueOffset field and appends out-of-line payloads."""
  if field_type == _TIFF_LONG8 and inline_size == 4:
    raise ValueError('TIFF LONG8 requires BigTIFF (inline_size == 8)')
  if field_type == _TIFF_ASCII and not isinstance(value, bytes):
    raise TypeError(f'TIFF ASCII requires bytes, got {type(value).__name__}')

  if isinstance(value, _TailStripOffsets):
    _, elem_size = _TIFF_PACK_FORMATS[value.field_type]
    total_size = value.count * elem_size
    if total_size <= inline_size:
      if tail_slots is not None:
        tail_slots.append((False, entry_val_pos, value))
      return b'\x00' * inline_size
    _align_even(payloads)
    slot_pos = len(payloads)
    offset = payload_base + slot_pos
    if tail_slots is not None:
      tail_slots.append((True, slot_pos, value))
    payloads.extend(b'\x00' * total_size)
    fmt = f'{endian}I' if inline_size == 4 else f'{endian}Q'
    return struct.pack(fmt, offset)

  if isinstance(value, bytes):
    if len(value) <= inline_size:
      return value.ljust(inline_size, b'\x00')
    _align_even(payloads)
    offset = payload_base + len(payloads)
    payloads.extend(value)
    fmt = f'{endian}I' if inline_size == 4 else f'{endian}Q'
    return struct.pack(fmt, offset)

  # Integer scalar inline value: TIFF requires left-justification.
  if field_type == _TIFF_SHORT:
    return struct.pack(f'{endian}H', value).ljust(inline_size, b'\x00')
  if field_type == _TIFF_BYTE:
    return struct.pack('B', value).ljust(inline_size, b'\x00')
  if field_type == _TIFF_LONG:
    return struct.pack(f'{endian}I', value).ljust(inline_size, b'\x00')
  if field_type == _TIFF_LONG8:
    return struct.pack(f'{endian}Q', value)
  raise ValueError(f'Unsupported scalar TIFF field type: {field_type}')


def _validate_sorted_tags(
    entries: Sequence[tuple[int, int, int, int | bytes | _TailStripOffsets]],
    allow_unsorted: bool,
) -> None:
  if allow_unsorted:
    return
  tags = [tag for tag, _, _, _ in entries]
  if tags != sorted(set(tags)):
    raise ValueError(f'TIFF IFD tags must be strictly ascending: {tags}')


def _patch_tail_slots(
    tail_slots: Sequence[tuple[bool, int, _TailStripOffsets]],
    tail_offset: int,
    endian: str,
    ifd_buf: bytearray,
    payloads: bytearray,
) -> None:
  for is_out_of_line, pos, spec in tail_slots:
    packed = _pack_strile_offsets(spec, tail_offset, endian)
    target_buf = payloads if is_out_of_line else ifd_buf
    target_buf[pos : pos + len(packed)] = packed


def _build_classic_tiff(
    entries: Sequence[tuple[int, int, int, int | bytes | _TailStripOffsets]],
    endian: str = '<',
    tail_bytes: bytes = b'',
    second_ifd: (
        Sequence[tuple[int, int, int, int | bytes | _TailStripOffsets]] | None
    ) = None,
    allow_unsorted: bool = False,
) -> bytes:
  """Builds a minimal classic 32-bit TIFF binary image in memory."""
  _validate_sorted_tags(entries, allow_unsorted)
  magic = b'II*\x00' if endian == '<' else b'MM\x00*'
  ifd_offset = 8
  payload_base = ifd_offset + 2 + len(entries) * 12 + 4
  payloads = bytearray()
  tail_slots1: list[tuple[bool, int, _TailStripOffsets]] = []

  ifd_buf = bytearray(struct.pack(f'{endian}H', len(entries)))
  for tag, field_type, count, value in entries:
    ifd_buf.extend(struct.pack(f'{endian}HHI', tag, field_type, count))
    val_bytes = _pack_inline_or_offset(
        value,
        field_type,
        endian,
        4,
        payloads,
        payload_base,
        tail_slots1,
        len(ifd_buf),
    )
    ifd_buf.extend(val_bytes)

  if second_ifd is None:
    next_ifd = 0
    ifd2_buf = bytearray()
    payloads2 = bytearray()
    tail_slots2: list[tuple[bool, int, _TailStripOffsets]] = []
  else:
    _validate_sorted_tags(second_ifd, allow_unsorted)
    _align_even(payloads)
    next_ifd = payload_base + len(payloads)
    payload_base2 = next_ifd + 2 + len(second_ifd) * 12 + 4
    payloads2 = bytearray()
    tail_slots2 = []
    ifd2_buf = bytearray(struct.pack(f'{endian}H', len(second_ifd)))
    for tag, field_type, count, value in second_ifd:
      ifd2_buf.extend(struct.pack(f'{endian}HHI', tag, field_type, count))
      val_bytes = _pack_inline_or_offset(
          value,
          field_type,
          endian,
          4,
          payloads2,
          payload_base2,
          tail_slots2,
          len(ifd2_buf),
      )
      ifd2_buf.extend(val_bytes)
    ifd2_buf.extend(struct.pack(f'{endian}I', 0))

  ifd_buf.extend(struct.pack(f'{endian}I', next_ifd))
  if tail_bytes:
    if second_ifd is None:
      _align_even(payloads)
    else:
      _align_even(payloads2)
  tail_offset = (
      ifd_offset + len(ifd_buf) + len(payloads) + len(ifd2_buf) + len(payloads2)
  )
  _patch_tail_slots(tail_slots1, tail_offset, endian, ifd_buf, payloads)
  _patch_tail_slots(tail_slots2, tail_offset, endian, ifd2_buf, payloads2)
  return (
      magic
      + struct.pack(f'{endian}I', ifd_offset)
      + bytes(ifd_buf)
      + bytes(payloads)
      + bytes(ifd2_buf)
      + bytes(payloads2)
      + tail_bytes
  )


def _build_bigtiff(
    entries: Sequence[tuple[int, int, int, int | bytes | _TailStripOffsets]],
    endian: str = '<',
    tail_bytes: bytes = b'',
    allow_unsorted: bool = False,
) -> bytes:
  """Builds a minimal 64-bit BigTIFF binary image in memory."""
  _validate_sorted_tags(entries, allow_unsorted)
  magic = b'II+\x00' if endian == '<' else b'MM\x00+'
  header = magic + struct.pack(f'{endian}HHQ', 8, 0, 16)
  payload_base = 16 + 8 + len(entries) * 20 + 8
  payloads = bytearray()
  tail_slots: list[tuple[bool, int, _TailStripOffsets]] = []

  ifd_buf = bytearray(struct.pack(f'{endian}Q', len(entries)))
  for tag, field_type, count, value in entries:
    ifd_buf.extend(struct.pack(f'{endian}HHQ', tag, field_type, count))
    val_bytes = _pack_inline_or_offset(
        value,
        field_type,
        endian,
        8,
        payloads,
        payload_base,
        tail_slots,
        len(ifd_buf),
    )
    ifd_buf.extend(val_bytes)
  ifd_buf.extend(struct.pack(f'{endian}Q', 0))
  if tail_bytes:
    _align_even(payloads)
  tail_offset = 16 + len(ifd_buf) + len(payloads)
  _patch_tail_slots(tail_slots, tail_offset, endian, ifd_buf, payloads)
  return header + bytes(ifd_buf) + bytes(payloads) + tail_bytes


def _minimal_1x1_entries(
    width: int = 1,
    height: int = 1,
    width_type: int = _TIFF_SHORT,
    height_type: int = _TIFF_SHORT,
    bits_per_sample: int | bytes = 8,
    bits_count: int = 1,
    compression: int = 1,
    photometric: int = 1,
    strip_offset: int | bytes | _TailStripOffsets = 0,
    strip_count: int = 1,
    strip_byte_count: int | bytes = 1,
    strip_byte_count_num: int = 1,
    samples_per_pixel: int = 1,
    rows_per_strip: int = 1,
    planar_config: int = 1,
    sample_format: int | bytes = 1,
    sample_format_count: int = 1,
    extra_entries: Sequence[
        tuple[int, int, int, int | bytes | _TailStripOffsets]
    ] = (),
) -> list[tuple[int, int, int, int | bytes | _TailStripOffsets]]:
  """Returns sorted classic TIFF IFD entries for a baseline raster."""
  entries: list[tuple[int, int, int, int | bytes | _TailStripOffsets]] = [
      (256, width_type, 1, width),
      (257, height_type, 1, height),
      (258, _TIFF_SHORT, bits_count, bits_per_sample),
      (259, _TIFF_SHORT, 1, compression),
      (262, _TIFF_SHORT, 1, photometric),
      (273, _TIFF_LONG, strip_count, strip_offset),
      (277, _TIFF_SHORT, 1, samples_per_pixel),
      (278, _TIFF_SHORT, 1, rows_per_strip),
      (279, _TIFF_LONG, strip_byte_count_num, strip_byte_count),
      (284, _TIFF_SHORT, 1, planar_config),
      (339, _TIFF_SHORT, sample_format_count, sample_format),
  ]
  entries.extend(extra_entries)
  entries.sort(key=lambda entry: entry[0])
  return entries


class TestReadGtiffInfo(gdrivers_util.DriverTestCase):
  """Golden JSON info checks for GeoTIFF data types."""

  def setUp(self, driver_name: str = DRIVER, ext: str = EXT) -> None:
    super().setUp(driver_name, ext)
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())

  def test_driver(self) -> None:
    self.CheckDriver()

  def test_info(self) -> None:
    bases = (
        'byte',
        'int10',
        'int12',
        'int16',
        'int24',
        'int32',
        'int64',
        'uint16',
        'uint32',
        'uint64',
        'cint16',
        'cint32',
        'float16',
        'float24',
        'float32',
        'float64',
        'cfloat32',
        'cfloat64',
        'float32_minwhite',
        'minfloat',
        'rgba-float32',
        'rgba-float64',
        'rgba-cfloat64',
        # Earth Engine export style.
        'float32-guuu',
    )
    for base in bases:
      with self.subTest(base=base):
        filepath = gcore_util.GetTestFilePath(os.path.join(DRIVER, base + EXT))
        self.assertTrue(pathlib.Path(filepath).exists(), filepath)
        self.CheckOpen(filepath)
        self.CheckInfo()


class TestReadGtiff(parameterized.TestCase, gdaltest.TestCase):
  """Tests for GeoTIFF reading ported from upstream tiff_read.py."""

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(ogr.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    driver = gdal.GetDriverByName(DRIVER)
    if driver is None:
      self.fail(f'Missing GDAL driver: {DRIVER}')
    self.driver: gdal.Driver = driver
    self._byte_arr: np.ndarray | None = None
    gdal.ErrorReset()

  def _vsimem_dir(self, suffix: str) -> str:
    safe_id = ''.join(c if c.isalnum() or c in '_-' else '_' for c in self.id())
    return self.enter_context(
        gcore_util.VsimemCleanupDir(
            f'/vsimem/tiff_read_test/{safe_id}/{suffix}'
        )
    )

  def _open_readonly(
      self, filepath: str, open_options: Sequence[str] = ()
  ) -> gdal.Dataset:
    if open_options:
      ds = gdal.OpenEx(
          filepath,
          gdal.OF_RASTER | gdal.OF_READONLY,
          open_options=list(open_options),
      )
    else:
      ds = gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY)
    if ds is None:
      self.fail(f'Failed to open dataset: {filepath}')
    return ds

  def _require_srs(self, ds: gdal.Dataset) -> osr.SpatialReference:
    srs = ds.GetSpatialRef()
    if srs is None:
      self.fail('Expected non-None SpatialReference')
    return srs

  def _require_band_rat(self, band: gdal.Band) -> gdal.RasterAttributeTable:
    rat = band.GetDefaultRAT()
    if rat is None:
      self.fail('Expected non-None RasterAttributeTable')
    return rat

  def _require_overview(self, band: gdal.Band, index: int) -> gdal.Band:
    ovr = band.GetOverview(index)
    if ovr is None:
      self.fail(f'Expected overview {index}')
    return ovr

  def _require_driver_by_name(self, name: str) -> gdal.Driver:
    drv = gdal.GetDriverByName(name)
    if drv is None:
      self.fail(f'Missing GDAL driver: {name}')
    return drv

  def _test_path(self, filename: str) -> str:
    path = pathlib.Path(gcore_util.GetTestFilePath(filename))
    self.assertTrue(path.exists(), f'Missing testdata file: {path}')
    return str(path)

  def _copy_to_vsimem(self, src_rel: str, dst_vsimem: str) -> None:
    src_path = self._test_path(src_rel)
    with open(src_path, 'rb') as f:
      gdal.FileFromMemBuffer(dst_vsimem, f.read())

  def _expected_byte_array(self) -> np.ndarray:
    if self._byte_arr is None:
      with self._open_readonly(self._test_path('byte.tif')) as ref_ds:
        arr = ref_ds.GetRasterBand(1).ReadAsArray()
        if arr is None:
          self.fail('Failed to read byte.tif pixel array')
        self._byte_arr = arr
    return self._byte_arr

  def _assert_band(
      self,
      band: gdal.Band,
      expected_checksum: int,
      expected_shape: tuple[int, int],
      expected_dtype: type[np.generic],
      gdal_dtype: int = gdal.GDT_Byte,
  ) -> np.ndarray:
    self.assertEqual(band.DataType, gdal_dtype)
    self.assertEqual(band.Checksum(), expected_checksum)
    arr = band.ReadAsArray()
    if arr is None:
      self.fail('Expected non-None ReadAsArray() result')
    self.assertEqual(arr.shape, expected_shape)
    self.assertEqual(arr.dtype, np.dtype(expected_dtype))
    return arr

  def _assert_byte_matches(self, band: gdal.Band) -> None:
    self.assertEqual(band.DataType, gdal.GDT_Byte)
    self.assertEqual(band.Checksum(), 4672)
    np.testing.assert_array_equal(
        band.ReadAsArray(), self._expected_byte_array()
    )

  def _assert_geotransform(
      self,
      ds: gdal.Dataset,
      expected: Sequence[float],
      atol: float = 1e-6,
  ) -> None:
    np.testing.assert_allclose(
        ds.GetGeoTransform(), expected, rtol=0.0, atol=atol
    )

  def _assert_projection_same(
      self, ds: gdal.Dataset, expected_wkt: str
  ) -> None:
    actual_srs = self._require_srs(ds)
    expected_srs = osr.SpatialReference()
    expected_srs.SetFromUserInput(expected_wkt)
    self.assertIsSame(actual_srs, expected_srs)

  # Upstream: test_tiff_open
  @parameterized.named_parameters(
      dict(
          testcase_name='uint16_sgilog',
          filename='uint16_sgilog.tif',
          band_num=1,
          checksum=4672,
      ),
      dict(
          testcase_name='next_literalrow',
          filename='next_literalrow.tif',
          band_num=1,
          checksum=4,
      ),
      dict(
          testcase_name='next_literalspan',
          filename='next_literalspan.tif',
          band_num=1,
          checksum=4,
      ),
      dict(
          testcase_name='next_default_case',
          filename='next_default_case.tif',
          band_num=1,
          checksum=4,
      ),
      dict(
          testcase_name='thunder',
          filename='thunder.tif',
          band_num=1,
          checksum=3,
      ),
      dict(
          testcase_name='separate_tiled',
          filename='separate_tiled.tif',
          band_num=2,
          checksum=15234,
      ),
      dict(
          testcase_name='seperate_strip',
          filename='seperate_strip.tif',
          band_num=2,
          checksum=15234,
      ),
      dict(
          testcase_name='contig_tiled',
          filename='contig_tiled.tif',
          band_num=2,
          checksum=15234,
      ),
      dict(
          testcase_name='contig_strip',
          filename='contig_strip.tif',
          band_num=2,
          checksum=15234,
      ),
      dict(
          testcase_name='float32_lzw_predictor_3_big_endian',
          filename='gtiff/float32_lzw_predictor_3_big_endian.tif',
          band_num=1,
          checksum=4672,
      ),
  )
  def test_open_additional_formats(
      self, filename: str, band_num: int, checksum: int
  ) -> None:
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      driver = ds.GetDriver()
      if driver is None:
        self.fail('Expected non-None GDAL driver')
      self.assertEqual(driver.ShortName, 'GTiff')
      band = ds.GetRasterBand(band_num)
      self.assertEqual(band.Checksum(), checksum)
      arr = band.ReadAsArray()
      if arr is None:
        self.fail('Expected non-None ReadAsArray() result')
      self.assertEqual(arr.shape, (ds.RasterYSize, ds.RasterXSize))

  # Upstream: test_tiff_open
  # (empty1bit.tif, int64_full_range, uint64_full_range)
  def test_open_synthesized_edge_formats(self) -> None:
    vsi_dir = self._vsimem_dir('edge_formats')
    with self.subTest(case='empty_1bit'):
      empty_path = f'{vsi_dir}/empty1bit.tif'
      entries = _minimal_1x1_entries(
          width=10,
          height=10,
          bits_per_sample=1,
          rows_per_strip=10,
          strip_offset=0,
          strip_byte_count=0,
      )
      gdal.FileFromMemBuffer(empty_path, _build_classic_tiff(entries))
      with gdal.OpenEx(empty_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        band = ds.GetRasterBand(1)
        self.assertEqual(band.Checksum(), 0)
        np.testing.assert_array_equal(
            band.ReadAsArray(), np.zeros((10, 10), dtype=np.uint8)
        )

    with self.subTest(case='int64_full_range'):
      i64_path = f'{vsi_dir}/int64_full_range.tif'
      entries = _minimal_1x1_entries(
          width=2,
          height=1,
          bits_per_sample=64,
          strip_offset=_TailStripOffsets(),
          strip_byte_count=16,
          sample_format=2,
      )
      pixels = struct.pack('<qq', -10000000000, 10000000000)
      gdal.FileFromMemBuffer(
          i64_path, _build_classic_tiff(entries, tail_bytes=pixels)
      )
      with self._open_readonly(i64_path) as ds:
        band = ds.GetRasterBand(1)
        self.assertEqual(band.DataType, gdal.GDT_Int64)
        self.assertEqual(band.Checksum(), 65535)
        np.testing.assert_array_equal(
            band.ReadAsArray(),
            np.array([[-10000000000, 10000000000]], np.int64),
        )

    with self.subTest(case='uint64_full_range'):
      u64_path = f'{vsi_dir}/uint64_full_range.tif'
      entries = _minimal_1x1_entries(
          width=2,
          height=1,
          bits_per_sample=64,
          strip_offset=_TailStripOffsets(),
          strip_byte_count=16,
          sample_format=1,
      )
      pixels = struct.pack('<QQ', 0, 10000000000)
      gdal.FileFromMemBuffer(
          u64_path, _build_classic_tiff(entries, tail_bytes=pixels)
      )
      with self._open_readonly(u64_path) as ds:
        band = ds.GetRasterBand(1)
        self.assertEqual(band.DataType, gdal.GDT_UInt64)
        self.assertEqual(band.Checksum(), 1)
        np.testing.assert_array_equal(
            band.ReadAsArray(), np.array([[0, 10000000000]], np.uint64)
        )

  # Upstream: test_tiff_read_off
  @parameterized.named_parameters(
      dict(testcase_name='plain', prefix=''),
      dict(testcase_name='dir_offset', prefix='GTIFF_DIR:off:408:'),
      dict(
          testcase_name='raw_dir_offset', prefix='GTIFF_RAW:GTIFF_DIR:off:408:'
      ),
      dict(testcase_name='dir_index', prefix='GTIFF_DIR:1:'),
  )
  def test_tiff_read_offset(self, prefix: str) -> None:
    basename = self._test_path('byte.tif')
    with gdal.OpenEx(basename, gdal.OF_RASTER | gdal.OF_READONLY) as ref_ds:
      expected_arr = ref_ds.GetRasterBand(1).ReadAsArray()
    with gdal.OpenEx(
        prefix + basename, gdal.OF_RASTER | gdal.OF_READONLY
    ) as ds:
      self._assert_geotransform(ds, BYTE_GEOTRANSFORM)
      band = ds.GetRasterBand(1)
      self.assertEqual(band.DataType, gdal.GDT_Byte)
      self.assertEqual(band.Checksum(), 4672)
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)

  # Upstream: test_tiff_read_off_errors
  @parameterized.named_parameters(
      dict(
          testcase_name='empty_spec',
          spec='GTIFF_DIR:',
          err_regex=r'No such file or directory',
      ),
      dict(
          testcase_name='missing_colon_and_file',
          spec='GTIFF_DIR:1',
          err_regex=r'Unable to extract offset or filename',
      ),
      dict(
          testcase_name='empty_file',
          spec='GTIFF_DIR:1:',
          err_regex=r'Unable to extract offset or filename',
      ),
      dict(
          testcase_name='nonexistent_file',
          spec='GTIFF_DIR:1:/vsimem/i_dont_exist.tif',
          err_regex=r'No such file or directory',
      ),
      dict(
          testcase_name='dir_not_found',
          spec='GTIFF_DIR:2:{byte}',
          err_regex=r'Requested directory 2 not found',
      ),
  )
  def test_tiff_read_offset_errors(self, spec: str, err_regex: str) -> None:
    target = spec.format(byte=self._test_path('byte.tif'))
    with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, err_regex):
      gdal.OpenEx(target, gdal.OF_RASTER | gdal.OF_READONLY)

  # Upstream: test_tiff_read_off_error_update
  def test_tiff_read_offset_update_switches_to_readonly(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tmp_tif = tmp_dir / 'byte.tif'
    shutil.copyfile(self._test_path('byte.tif'), tmp_tif)
    target = f'GTIFF_DIR:1:{tmp_tif}'
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.OpenEx(target, gdal.OF_RASTER | gdal.OF_UPDATE) as ds:
        err_type = gdal.GetLastErrorType()
        err_msg = gdal.GetLastErrorMsg()
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    self.assertEqual(err_type, gdal.CE_Warning)
    self.assertIn('Switching to read-only', err_msg)

  # Upstream: test_tiff_check_alpha
  def test_grey_alpha(self) -> None:
    filepath = self._test_path('stefan_full_greyalpha.tif')
    with self._open_readonly(filepath) as ds:
      self._assert_geotransform(ds, EMPTY_GEOTRANSFORM)
      self.assertEqual(ds.RasterCount, 2)
      self._assert_band(ds.GetRasterBand(1), 1970, (150, 162), np.uint8)
      self._assert_band(ds.GetRasterBand(2), 10807, (150, 162), np.uint8)
      self.assertEqual(
          ds.GetRasterBand(2).GetRasterColorInterpretation(),
          gdal.GCI_AlphaBand,
      )

  def test_rgb_alpha(self) -> None:
    filepath = self._test_path('stefan_full_rgba.tif')
    with self._open_readonly(filepath) as ds:
      self._assert_geotransform(ds, EMPTY_GEOTRANSFORM)
      self.assertEqual(ds.RasterCount, 4)
      for i, checksum in enumerate((12603, 58561, 36064, 10807)):
        with self.subTest(band=i + 1):
          self._assert_band(
              ds.GetRasterBand(i + 1), checksum, (150, 162), np.uint8
          )
      self.assertEqual(
          ds.GetRasterBand(4).GetRasterColorInterpretation(),
          gdal.GCI_AlphaBand,
      )

  def test_rgb_undefined(self) -> None:
    filepath = self._test_path('stefan_full_rgba_photometric_rgb.tif')
    with self._open_readonly(filepath) as ds:
      self._assert_geotransform(ds, EMPTY_GEOTRANSFORM)
      self.assertEqual(ds.RasterCount, 4)
      for i, checksum in enumerate((12603, 58561, 36064, 10807)):
        with self.subTest(band=i + 1):
          self._assert_band(
              ds.GetRasterBand(i + 1), checksum, (150, 162), np.uint8
          )
      self.assertEqual(
          ds.GetRasterBand(4).GetRasterColorInterpretation(),
          gdal.GCI_Undefined,
      )

  # Upstream: test_tiff_read_minisblack_as_rgba / colortable / strip_separate /
  # tiled_separate / logl_as_rgba. GTIFF_FORCE_RGBA is guarded by #ifdef DEBUG
  # in gtiffdataset_read.cpp, so non-DEBUG Google3 builds keep native band
  # counts (and uint16_sgilog.tif is also 16-bit, > 8 bits/sample).
  def test_force_rgba_disabled_in_non_debug_build(self) -> None:
    vsi_dir = self._vsimem_dir('force_rgba')
    palette_tif = f'{vsi_dir}/palette.tif'
    with self.driver.Create(palette_tif, 8, 8, 1, gdal.GDT_Byte) as ds:
      ct = gdal.ColorTable()
      ct.SetColorEntry(0, (10, 20, 30, 255))
      ds.GetRasterBand(1).SetColorTable(ct)

    with gdal.config_option('GTIFF_FORCE_RGBA', 'YES'):
      with self.subTest(case='minisblack'):
        with self._open_readonly(self._test_path('byte.tif')) as ds:
          self.assertEqual(ds.RasterCount, 1)
      with self.subTest(case='greyalpha'):
        with self._open_readonly(
            self._test_path('stefan_full_greyalpha.tif')
        ) as ds:
          self.assertEqual(ds.RasterCount, 2)
      with self.subTest(case='colortable'):
        with self._open_readonly(palette_tif) as ds:
          self.assertEqual(ds.RasterCount, 1)
      with self.subTest(case='rgb_separate'):
        sep_tif = f'{vsi_dir}/rgbsmall_sep.tif'
        with gdal.Translate(
            sep_tif,
            self._test_path('rgbsmall.tif'),
            creationOptions=['INTERLEAVE=BAND'],
        ):
          pass
        with self._open_readonly(sep_tif) as ds:
          self.assertEqual(ds.RasterCount, 3)
      with self.subTest(case='logl_16bit'):
        with self._open_readonly(self._test_path('uint16_sgilog.tif')) as ds:
          self.assertEqual(ds.RasterCount, 1)
          self._assert_band(
              ds.GetRasterBand(1), 4672, (20, 20), np.int16, gdal.GDT_Int16
          )

  def test_cmyk_rgba(self) -> None:
    filepath = self._test_path('rgbsmall_cmyk.tif')
    with self._open_readonly(filepath) as ds:
      self._assert_geotransform(ds, EMPTY_GEOTRANSFORM)
      self.assertEqual(ds.RasterCount, 4)
      self.assertEqual(
          ds.GetMetadata('IMAGE_STRUCTURE'),
          {'INTERLEAVE': 'PIXEL', 'SOURCE_COLOR_SPACE': 'CMYK'},
      )
      rgba_bands = (
          gdal.GCI_RedBand,
          gdal.GCI_GreenBand,
          gdal.GCI_BlueBand,
          gdal.GCI_AlphaBand,
      )
      expected_cs = (23303, 25101, 8782, 30658)
      for idx, (checksum, band_type) in enumerate(
          zip(expected_cs, rgba_bands, strict=True)
      ):
        with self.subTest(band=idx + 1):
          band = ds.GetRasterBand(idx + 1)
          self._assert_band(band, checksum, (50, 50), np.uint8)
          self.assertEqual(band.GetRasterColorInterpretation(), band_type)

  def test_cmyk_raw(self) -> None:
    filepath = self._test_path('rgbsmall_cmyk.tif')
    with self._open_readonly('GTIFF_RAW:' + filepath) as ds:
      self._assert_band(ds.GetRasterBand(1), 29430, (50, 50), np.uint8)
      cmyk_bands = (
          gdal.GCI_CyanBand,
          gdal.GCI_MagentaBand,
          gdal.GCI_YellowBand,
          gdal.GCI_BlackBand,
      )
      for idx, band_type in enumerate(cmyk_bands):
        with self.subTest(band=idx + 1):
          band = ds.GetRasterBand(idx + 1)
          self.assertEqual(band.GetRasterColorInterpretation(), band_type)

  # Upstream: test_tiff_read_ojpeg
  # OJPEG_SUPPORT is disabled in //third_party/tiff/libtiff/tiffconf.h.
  def test_ojpeg_disabled(self) -> None:
    filepath = self._test_path('zackthecat.tif')
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, r'missing codec|Old-JPEG|OJPEG'),
    ):
      self._open_readonly(filepath)

  # Upstream: test_tiff_read_gzip
  def test_gzip(self) -> None:
    vsi_dir = self._vsimem_dir('gzip')
    gz_path = f'{vsi_dir}/byte.tif.gz'
    self._copy_to_vsimem('byte.tif.gz', gz_path)
    with self._open_readonly('/vsigzip/' + gz_path) as ds:
      self._assert_geotransform(ds, BYTE_GEOTRANSFORM)
      self._assert_byte_matches(ds.GetRasterBand(1))
    self.assertIsNone(gdal.VSIStatL(f'{gz_path}.properties'))

  # Upstream: test_tiff_read_zip_1..5
  @parameterized.named_parameters(
      dict(testcase_name='zip_explicit', subpath='byte.tif.zip/byte.tif'),
      dict(testcase_name='zip_implicit', subpath='byte.tif.zip'),
      dict(
          testcase_name='one_file_subdir_explicit',
          subpath='onefileinsubdir.zip/onefileinsubdir/byte.tif',
      ),
      dict(
          testcase_name='one_file_subdir_implicit',
          subpath='onefileinsubdir.zip',
      ),
      dict(
          testcase_name='two_files_subdir_explicit',
          subpath='twofileinsubdir.zip/twofileinsubdir/byte.tif',
      ),
  )
  def test_zips(self, subpath: str) -> None:
    zip_name = subpath.split('/')[0]
    self._test_path(zip_name)
    filepath = gcore_util.GetTestFilePath(subpath)
    with self._open_readonly('/vsizip/' + filepath) as ds:
      self._assert_geotransform(ds, BYTE_GEOTRANSFORM)
      self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_tar_1..2, test_tiff_read_tgz_1..2
  @parameterized.named_parameters(
      dict(testcase_name='tar_explicit', subpath='byte.tar/byte.tif'),
      dict(testcase_name='tar_implicit', subpath='byte.tar'),
      dict(testcase_name='tgz_explicit', subpath='byte.tgz/byte.tif'),
      dict(testcase_name='tgz_implicit', subpath='byte.tgz'),
  )
  def test_tars(self, subpath: str) -> None:
    vsi_dir = self._vsimem_dir('tars')
    archive_name = subpath.split('/')[0]
    vsi_archive = f'{vsi_dir}/{archive_name}'
    self._copy_to_vsimem(archive_name, vsi_archive)
    inner = subpath[len(archive_name) :]
    with self._open_readonly(f'/vsitar/{vsi_archive}{inner}') as ds:
      self._assert_geotransform(ds, BYTE_GEOTRANSFORM)
      self._assert_byte_matches(ds.GetRasterBand(1))

  def test_grads(self) -> None:
    """Non-degree angular units (#601)."""
    filepath = self._test_path('test_gf.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self._assert_geotransform(ds, (827000.0, 0.5, 0.0, 2224000.0, 0.0, -0.5))
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 1218)
      prj = (
          'PROJCS["NTF (Paris) / Lambert zone II",'
          '   GEOGCS["NTF (Paris)",'
          '       DATUM["Nouvelle_Triangulation_Francaise_Paris",'
          '           SPHEROID["Clarke 1880 (IGN)",6378249.2,293.466021293627,'
          '               AUTHORITY["EPSG","7011"]],'
          '           TOWGS84[-168,-60,320,0,0,0,0],'
          '           AUTHORITY["EPSG","6807"]],'
          '       PRIMEM["Paris",2.33722916999999,'
          '           AUTHORITY["EPSG","8903"]],'
          '       UNIT["grad",0.0157079632679489,'
          '           AUTHORITY["EPSG","9105"]],'
          '       AUTHORITY["EPSG","4807"]],'
          '   PROJECTION["Lambert_Conformal_Conic_1SP"],'
          '   PARAMETER["latitude_of_origin",52],'
          '   PARAMETER["central_meridian",0],'
          '   PARAMETER["scale_factor",0.99987742],'
          '   PARAMETER["false_easting",600000],'
          '   PARAMETER["false_northing",2200000],'
          '   UNIT["metre",1,'
          '       AUTHORITY["EPSG","9001"]],'
          '   AXIS["Easting",EAST],'
          '   AXIS["Northing",NORTH],'
          '   AUTHORITY["EPSG","27572"]]'
      )
      with gdal.quiet_errors():
        # Deprecated EPSG:27582 warns and resolves to EPSG:27572.
        self._assert_projection_same(ds, prj)

  # Upstream test_tiff_citation requires ESRI_BUILD=YES (ArcGIS Pro only).

  def test_linear_parm_units_correct(self) -> None:
    filepath = self._test_path('spaf27_correct.tif')
    with self._open_readonly(filepath) as ds:
      srs = self._require_srs(ds)
      self.assertAlmostEqual(
          srs.GetProjParm(osr.SRS_PP_FALSE_EASTING), 2000000.0, delta=0.001
      )

  def test_linear_parm_units_broken(self) -> None:
    filepath = self._test_path('spaf27_brokengdal.tif')
    with self._open_readonly(filepath) as ds:
      srs = self._require_srs(ds)
      self.assertAlmostEqual(
          srs.GetProjParm(osr.SRS_PP_FALSE_EASTING),
          609601.219202438,
          delta=0.001,
      )

  def test_linear_parm_units_epsg(self) -> None:
    filepath = self._test_path('spaf27_epsg.tif')
    with self._open_readonly(filepath) as ds:
      srs = self._require_srs(ds)
      self.assertAlmostEqual(
          srs.GetProjParm(osr.SRS_PP_FALSE_EASTING), 2000000.0, delta=0.001
      )

  @parameterized.named_parameters(
      dict(
          testcase_name='correct',
          filename='spaf27_correct.tif',
          expected_easting=6561666.66667,
      ),
      dict(
          testcase_name='markedcorrect',
          filename='spaf27_markedcorrect.tif',
          expected_easting=2000000.0,
      ),
      dict(
          testcase_name='brokengdal',
          filename='spaf27_brokengdal.tif',
          expected_easting=2000000.0,
      ),
  )
  def test_linear_parm_units_broken_option(
      self, filename: str, expected_easting: float
  ) -> None:
    filepath = self._test_path(filename)
    with gdal.config_option('GTIFF_LINEAR_UNITS', 'BROKEN'):
      with self._open_readonly(filepath) as ds:
        srs = self._require_srs(ds)
        self.assertAlmostEqual(
            srs.GetProjParm(osr.SRS_PP_FALSE_EASTING),
            expected_easting,
            delta=0.001,
        )

  def test_g4_split(self) -> None:
    filepath = self._test_path('slim_g4.tif')
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.GetBlockSize(), [1000, 1])
      self._assert_band(band, 3322, (7200, 1000), np.uint8)

  def test_multi_images(self) -> None:
    filepath = self._test_path('twoimages.tif')
    with self._open_readonly(filepath) as ds:
      self._assert_byte_matches(ds.GetRasterBand(1))
      md_expected = {
          'SUBDATASET_1_NAME': f'GTIFF_DIR:1:{filepath}',
          'SUBDATASET_1_DESC': 'Page 1 (20P x 20L x 1B)',
          'SUBDATASET_2_NAME': f'GTIFF_DIR:2:{filepath}',
          'SUBDATASET_2_DESC': 'Page 2 (20P x 20L x 1B)',
      }
      self.assertEqual(ds.GetMetadata('SUBDATASETS'), md_expected)

    for filenum in (1, 2):
      with self.subTest(subdataset=filenum):
        with self._open_readonly(f'GTIFF_DIR:{filenum}:{filepath}') as ds:
          self._assert_byte_matches(ds.GetRasterBand(1))

  def test_vsi_mem(self) -> None:
    vsi_dir = self._vsimem_dir('vsi_mem')
    filepath = f'{vsi_dir}/tiffinmem'
    self._copy_to_vsimem('byte.tif', filepath)

    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_UPDATE) as ds:
      self._assert_byte_matches(ds.GetRasterBand(1))
      ds.GetRasterBand(1).Fill(0)

    with self._open_readonly(filepath) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), np.zeros((20, 20), dtype=np.uint8)
      )

    backslash_path = f'{vsi_dir}\\tiffinmem'
    with self._open_readonly(backslash_path) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)

  def test_vsi_zip_mem(self) -> None:
    vsi_dir = self._vsimem_dir('vsi_zip_mem')
    filepath = f'{vsi_dir}/tiffinmem.zip'
    self._copy_to_vsimem('byte.tif.zip', filepath)
    with self._open_readonly(f'/vsizip/{filepath}/byte.tif') as ds:
      self._assert_byte_matches(ds.GetRasterBand(1))

  def test_tiff_projected_cs_type_geokey_only(self) -> None:
    filepath = self._test_path('ticket3019.tif')
    with self._open_readonly(filepath) as ds:
      self.assertIn('WGS 84 / UTM zone 31N', ds.GetProjection())
      srs = self._require_srs(ds)
      self.assertEqual(srs.GetAuthorityCode(None), '32631')

  def test_model_type_geokey_only(self) -> None:
    filepath = self._test_path('ModelTypeGeoKey_only.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      prj = (
          'LOCAL_CS["unnamed",'
          '  GEOGCS["unknown",'
          '   DATUM["unknown",'
          '    SPHEROID["unretrievable - using WGS84",6378137,298.257223563]],'
          '   PRIMEM["Greenwich",0],'
          '   UNIT[,0.0174532925199433]],'
          '  UNIT["unknown",1]]'
      )
      self._assert_projection_same(ds, prj)

  # Upstream: test_tiff_12bitjpeg. JPEG_DUAL_MODE_8_12 is disabled in
  # //third_party/tiff/libtiff/tif_config.h.
  def test_tiff_12bitjpeg_disabled(self) -> None:
    filepath = self._test_path('mandrilmini_12bitjpeg.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'JPEGPreDecode:Improper JPEG data precision'
          ),
      ):
        ds.GetRasterBand(1).ReadRaster(0, 0, 1, 1)

  # Upstream: test_tiff_read_stats_from_pam
  def test_tiff_read_stats_from_pam(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tmp_tif = tmp_dir / 'byte.tif'
    shutil.copyfile(self._test_path('byte.tif'), tmp_tif)

    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with gdal.OpenEx(str(tmp_tif), gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        band = ds.GetRasterBand(1)
        self.assertNotIn('STATISTICS_MINIMUM', band.GetMetadata())
        stats = band.GetStatistics(0, 1)
        np.testing.assert_allclose(
            stats, [74.0, 255.0, 126.765, 22.928470838676], atol=1e-3, rtol=0.0
        )

      pam_path = tmp_dir / 'byte.tif.aux.xml'
      self.assertTrue(pam_path.exists())

      with gdal.OpenEx(str(tmp_tif), gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        stats = ds.GetRasterBand(1).GetStatistics(0, 0)
        np.testing.assert_allclose(
            stats, [74.0, 255.0, 126.765, 22.928470838676], atol=1e-3, rtol=0.0
        )

  # Upstream: test_tiff_read_from_tab
  def test_tiff_read_from_tab(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    filepath = tmp_dir / 'read_from_tab.tif'
    tabpath = tmp_dir / 'read_from_tab.tab'
    with self.driver.Create(str(filepath), 1, 1):
      pass
    tabpath.write_text(
        '\n'.join((
            '!table',
            '!version 300',
            '!charset WindowsLatin1',
            '',
            'Definition Table',
            '  File "HP.TIF"',
            '  Type "RASTER"',
            '  (400000,1200000) (0,4000) Label "Pt 1",',
            '  (500000,1200000) (4000,4000) Label "Pt 2",',
            '  (500000,1300000) (4000,0) Label "Pt 3",',
            '  (400000,1300000) (0,0) Label "Pt 4"',
            (
                '  CoordSys Earth Projection 8, 79, "m", -2, 49, 0.9996012717, '
                '400000, -100000'
            ),
            '  Units "m"',
        )),
        encoding='utf-8',
    )
    with gdal.OpenEx(str(filepath), gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self._assert_geotransform(
          ds, (400000.0, 25.0, 0.0, 1300000.0, 0.0, -25.0)
      )
      self.assertIn('Ordnance_Survey_of_Great_Britain_1936', ds.GetProjection())
    self.driver.Delete(str(filepath))
    self.assertFalse(tabpath.exists())

  def test_tiff_read_pixelispoint(self) -> None:
    filepath = self._test_path('byte_point.tif')
    with gdal.config_option('GTIFF_POINT_GEO_IGNORE', 'FALSE'):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self._assert_geotransform(
            ds, (440690.0, 60.0, 0.0, 3751350.0, 0.0, -60.0)
        )
    with gdal.config_option('GTIFF_POINT_GEO_IGNORE', 'TRUE'):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self._assert_geotransform(ds, BYTE_GEOTRANSFORM)

  def test_tiff_read_geomatrix(self) -> None:
    filepath = self._test_path('geomatrix.tif')
    with gdal.config_option('GTIFF_POINT_GEO_IGNORE', 'FALSE'):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self._assert_geotransform(
            ds, (1841001.75, 1.5, -5.0, 1144003.25, -5.0, -1.5)
        )
    with gdal.config_option('GTIFF_POINT_GEO_IGNORE', 'TRUE'):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self._assert_geotransform(
            ds, (1841000.0, 1.5, -5.0, 1144000.0, -5.0, -1.5)
        )

  # Upstream: test_tiff_read_tiepoints_pixelispoint
  def test_tiff_read_tiepoints_pixelispoint(self) -> None:
    vsi_dir = self._vsimem_dir('tiepoints_point')
    filepath = f'{vsi_dir}/byte_gcp_pixelispoint.tif'
    tiepoints = struct.pack(
        '<24d',
        0.0,
        0.0,
        0.0,
        -180.0,
        90.0,
        0.0,
        20.0,
        0.0,
        0.0,
        180.0,
        90.0,
        0.0,
        20.0,
        20.0,
        0.0,
        180.0,
        -90.0,
        0.0,
        0.0,
        20.0,
        0.0,
        -180.0,
        -90.0,
        0.0,
    )
    # GeoKeys: version 1.1.0, 2 keys:
    # GTModelTypeGeoKey(1024)=2 (Geographic), GTRasterTypeGeoKey(1025)=2 (Pt).
    geokeys = struct.pack('<12H', 1, 1, 0, 2, 1024, 0, 1, 2, 1025, 0, 1, 2)
    entries = _minimal_1x1_entries(
        width=20,
        height=20,
        rows_per_strip=20,
        strip_offset=_TailStripOffsets(),
        strip_byte_count=400,
        extra_entries=(
            (33922, _TIFF_DOUBLE, 24, tiepoints),
            (34735, _TIFF_SHORT, 12, geokeys),
        ),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00' * 400)
    )

    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Point')
      self.assertEqual(ds.GetGCPCount(), 4)
      gcp = ds.GetGCPs()[0]
      self.assertAlmostEqual(gcp.GCPPixel, 0.5, delta=1e-5)
      self.assertAlmostEqual(gcp.GCPLine, 0.5, delta=1e-5)
      self.assertAlmostEqual(gcp.GCPX, -180.0, delta=1e-5)
      self.assertAlmostEqual(gcp.GCPY, 90.0, delta=1e-5)
      self.assertAlmostEqual(gcp.GCPZ, 0.0, delta=1e-5)

    with gdal.config_option('GTIFF_POINT_GEO_IGNORE', 'YES'):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self.assertEqual(ds.GetMetadataItem('AREA_OR_POINT'), 'Point')
        self.assertEqual(ds.GetGCPCount(), 4)
        gcp = ds.GetGCPs()[0]
        self.assertAlmostEqual(gcp.GCPPixel, 0.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPLine, 0.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPX, -180.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPY, 90.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPZ, 0.0, delta=1e-5)

  def test_corrupted_gtiff_tags(self) -> None:
    filepath = self._test_path('corrupted_gtiff_tags.tif')
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        err_msg = gdal.GetLastErrorMsg()
        self.assertEqual(ds.RasterXSize, 1)
    self.assertIn(
        'TIFFFetchNormalTag:IO error during reading of '
        '"GeoASCIIParams"; tag ignored',
        err_msg,
    )

  # Upstream: test_tiff_read_tag_without_null_byte
  def test_tiff_read_tag_without_null_byte(self) -> None:
    vsi_dir = self._vsimem_dir('tag_no_null')
    filepath = f'{vsi_dir}/tag_without_null_byte.tif'
    software_val = b'software without null byte '  # 27 bytes, no trailing NUL.
    entries = _minimal_1x1_entries(
        strip_offset=_TailStripOffsets(),
        strip_byte_count=1,
        extra_entries=((305, _TIFF_ASCII, len(software_val), software_val),),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00')
    )

    gdal.ErrorReset()
    with gdal.config_option('CPL_DEBUG', 'OFF'):
      with self._open_readonly(filepath) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
        self.assertEqual(gdal.GetLastErrorType(), gdal.CE_None)

  def test_tiff_read_buggy_packbits(self) -> None:
    filepath = self._test_path('byte_buggy_packbits.tif')
    with gdal.config_option('GTIFF_IGNORE_READ_ERRORS', None):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(RuntimeError, r'PackBitsDecode|Read failed'),
        ):
          ds.ReadRaster(0, 0, 20, 20)

    with gdal.config_option('GTIFF_IGNORE_READ_ERRORS', 'YES'):
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
          data = ds.ReadRaster(0, 0, 20, 20)
        self.assertIsNotNone(data)
        self.assertLen(data, 20 * 20)

  def test_tiff_read_rpc_txt(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tmp_tif = tmp_dir / 'test.tif'
    tmp_txt = tmp_dir / 'test_rpc.txt'
    shutil.copyfile(self._test_path('byte.tif'), tmp_tif)
    shutil.copyfile(self._test_path('test_rpc.txt'), tmp_txt)
    with gdal.OpenEx(str(tmp_tif), gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      metadata = ds.GetMetadata('RPC')
      self.assertEqual(metadata['HEIGHT_OFF'], '+0300.000 meters')
      coefs = [float(val) for val in metadata['LINE_DEN_COEFF'].split()[:2]]
      self.assertAlmostEqual(coefs[0], 1.0)
      self.assertAlmostEqual(coefs[1], -5.207696939454288e-03)

  def test_tiff_read_rpc_tif(self) -> None:
    filepath = self._test_path('byte_rpc.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      metadata = ds.GetMetadata('RPC')
      self.assertEqual(metadata['HEIGHT_OFF'], '300')
      self.assertTrue(
          metadata['LINE_DEN_COEFF'].startswith('1 -0.00520769693945429')
      )

  def test_tiff_small(self) -> None:
    vsi_dir = self._vsimem_dir('small')
    filepath = f'{vsi_dir}/small.tif'
    data = (
        b'\x49\x49\x2A\x00\x08\x00\x00\x00\x04\x00\x00\x01\x03\x00'
        b'\x01\x00\x00\x00\x01\x00\x00\x00\x01\x01\x03\x00\x01\x00'
        b'\x00\x00\x01\x00\x00\x00\x11\x01\x04\x00\x01\x00\x00\x00'
        b'\x00\x00\x00\x00\x17\x01\x04\x00\x01\x00\x00\x00\x01\x00'
        b'\x00\x00'
    )
    gdal.FileFromMemBuffer(filepath, data)
    with self._open_readonly(filepath) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 0)
      np.testing.assert_array_equal(
          ds.GetRasterBand(1).ReadAsArray(), np.zeros((1, 1), dtype=np.uint8)
      )

  def test_tiff_dos_strip_chop(self) -> None:
    filepath = self._test_path('dos_strip_chop.tif')
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self.assertIn('StripByteCounts', gdal.GetLastErrorMsg())
        with self.assertRaisesRegex(RuntimeError, r'TIFFReadEncodedStrip'):
          ds.GetRasterBand(1).Checksum()

  def test_tiff_read_exif_and_gps(self) -> None:
    filepath = self._test_path('exif_and_gps.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      expected_exif = {
          'EXIF_SpectralSensitivity': 'EXIF Spectral Sensitivity',
          'EXIF_ExifVersion': '0220',
          'EXIF_GPSVersionID': '0x02 0x02 0x00 0x00',
          'EXIF_GPSLongitude': '(34) (12) (0)',
          'EXIF_ComponentsConfiguration': '0x01 0x02 0x03 0x00',
          'EXIF_GPSLongitudeRef': 'E',
          'EXIF_GPSLatitudeRef': 'S',
          'EXIF_GPSLatitude': '(77) (5) (60)',
      }
      self.assertEqual(ds.GetMetadata('EXIF'), expected_exif)
      self.assertEqual(
          ds.GetMetadataItem('EXIF_GPSVersionID', 'EXIF'),
          '0x02 0x02 0x00 0x00',
      )

  def test_no_exif(self) -> None:
    filepath = self._test_path('byte.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEmpty(ds.GetMetadata('EXIF'))

  @parameterized.named_parameters(
      dict(
          testcase_name='pixel_interleaved',
          filename='stefan_full_rgba_jpeg_contig.tif',
          expected_interleave='PIXEL',
      ),
      dict(
          testcase_name='band_interleaved',
          filename='stefan_full_rgba_jpeg_separate.tif',
          expected_interleave='BAND',
      ),
  )
  def test_tiff_jpeg_rgba_interleaving(
      self, filename: str, expected_interleave: str
  ) -> None:
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      self.assertEqual(
          ds.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'),
          expected_interleave,
      )
      for i, checksum in enumerate([16404, 62700, 37913, 14174]):
        with self.subTest(band=i + 1):
          band = ds.GetRasterBand(i + 1)
          self._assert_band(band, checksum, (150, 162), np.uint8)
          self.assertEqual(
              band.GetRasterColorInterpretation(), i + gdal.GCI_RedBand
          )

  def test_ycbcr(self) -> None:
    # Port of test_tiff_read_online_1 using local testdata (no network allowed).
    filepath = self._test_path('imgpb17.tif')
    gdal.ErrorReset()
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      arr = self._assert_band(band, 62628, (3712, 3712), np.uint8)
      self.assertGreater(int(arr.max()), 0)
      self.assertEqual(gdal.GetLastErrorMsg(), '')

  # Upstream /vsicurl/ tests (test_tiff_read_vsicurl_multirange,
  # test_tiff_read_cog_vsicurl, test_tiff_read_cog_with_mask_vsicurl,
  # test_tiff_read_vsicurl_multi_threaded_beyond_advise_read_limit,
  # test_tiff_read_multi_threaded_vsicurl*,
  # test_tiff_read_overview_level_open_option_honor_..._EMPTY_DIR)
  # are omitted because HAVE_CURL is disabled and network access is forbidden.

  # Upstream: test_tiff_read_huge4GB
  def test_tiff_read_huge4gb(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    filepath = tmp_dir / 'huge4GB.tif'
    entries = _minimal_1x1_entries(
        width=65535,
        height=65535,
        rows_per_strip=65535,
        strip_offset=_TailStripOffsets(),
        strip_byte_count=65535 * 65535,
    )
    header = _build_classic_tiff(entries, tail_bytes=b'\x00' * (401 - 146))
    filepath.write_bytes(header)
    probe = tmp_dir / 'sparse_probe.bin'
    with open(probe, 'wb') as f:
      f.seek(1024 * 1024)
      f.write(b' ')
    probe_blocks = getattr(os.stat(probe), 'st_blocks', None)
    probe.unlink()
    if probe_blocks is not None and probe_blocks * 512 < 1024 * 1024:
      with open(filepath, 'rb+') as f:
        f.seek(65535 * 65535 + 401)
        f.write(b' ')
    with self._open_readonly(str(filepath)) as ds:
      self.assertEqual((ds.RasterXSize, ds.RasterYSize), (65535, 65535))

  # Upstream: test_tiff_read_bigtiff
  def test_tiff_read_bigtiff(self) -> None:
    vsi_dir = self._vsimem_dir('bigtiff')
    bigtiff_path = f'{vsi_dir}/byte_bigtiff_strip5lines.tif'
    with gdal.Translate(
        bigtiff_path,
        self._test_path('byte.tif'),
        creationOptions=['BIGTIFF=YES', 'BLOCKYSIZE=5'],
    ):
      pass
    with self._open_readonly(bigtiff_path) as ds:
      band = ds.GetRasterBand(1)
      self._assert_byte_matches(band)
      self.assertEqual(band.GetBlockSize(), [20, 5])

  # Upstream: test_tiff_read_tiff_metadata
  def test_tiff_read_tiff_metadata(self) -> None:
    contig_path = self._test_path('stefan_full_rgba_jpeg_contig.tif')
    with gdal.OpenEx(contig_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.GetMetadataItem('BLOCK_OFFSET_0_0', 'TIFF'), '254')
      self.assertEqual(band.GetMetadataItem('BLOCK_SIZE_0_0', 'TIFF'), '770')
      jpegtables = band.GetMetadataItem('JPEGTABLES', 'TIFF')
      self.assertIsNotNone(jpegtables)
      self.assertTrue(jpegtables.startswith('FFD8'))
      self.assertIsNone(band.GetMetadataItem('BLOCK_OFFSET_100_0', 'TIFF'))
      self.assertIsNone(band.GetMetadataItem('BLOCK_OFFSET_0_100', 'TIFF'))
      self.assertIsNone(band.GetMetadataItem('BLOCK_SIZE_100_0', 'TIFF'))
      self.assertIsNone(band.GetMetadataItem('BLOCK_SIZE_0_100', 'TIFF'))

    sep_path = self._test_path('stefan_full_rgba_jpeg_separate.tif')
    with gdal.OpenEx(sep_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band4 = ds.GetRasterBand(4)
      self.assertEqual(
          band4.GetMetadataItem('BLOCK_OFFSET_0_2', 'TIFF'), '11071'
      )
      self.assertEqual(band4.GetMetadataItem('BLOCK_SIZE_0_2', 'TIFF'), '188')

  # Upstream: test_tiff_read_irregular_tile_size_jpeg_in_tiff
  def test_tiff_read_irregular_tile_size_jpeg_in_tiff(self) -> None:
    filepath = self._test_path('irregular_tile_size_jpeg_in_tiff.tif')
    with self._open_readonly(filepath) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'JPEGPreDecode:JPEG strip/tile size exceeds expected dimensions',
          ),
      ):
        ds.GetRasterBand(1).Checksum()

  # Upstream: test_tiff_read_irregular_tile_size_jpeg_in_tiff_overview
  def test_tiff_read_irregular_tile_size_jpeg_in_tiff_overview(self) -> None:
    filepath = self._test_path('irregular_tile_size_jpeg_in_tiff.tif')
    with self._open_readonly(filepath) as ds:
      ovr = self._require_overview(ds.GetRasterBand(1), 0)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'libjpeg: Premature end of JPEG file'
          ),
      ):
        ovr.Checksum()

  def _create_direct_io_source(self, dt: int) -> tuple[gdal.Dataset, int]:
    """Creates a 4-band test dataset in MEM for DirectIO/VirtualMemIO."""
    with self._open_readonly(
        self._test_path('stefan_full_rgba.tif')
    ) as base_ds:
      mem_drv = self._require_driver_by_name('MEM')
      if dt == gdal.GDT_Byte:
        return mem_drv.CreateCopy('', base_ds), 1
      dt_size = 2 if dt == gdal.GDT_Int16 else 4
      mem_ds = mem_drv.Create(
          '', base_ds.RasterXSize, base_ds.RasterYSize, base_ds.RasterCount, dt
      )
      raw = base_ds.ReadRaster(
          0, 0, base_ds.RasterXSize, base_ds.RasterYSize, buf_type=dt
      )
      arr = np.frombuffer(raw, dtype=np.uint8).reshape(-1, dt_size).copy()
      if dt == gdal.GDT_Int16:
        arr[:, 1] = 255 - arr[:, 0]
      else:
        arr[:, 1] = arr[:, 0]
        arr[:, 2] = 255 - arr[:, 0]
        arr[:, 3] = 255 - arr[:, 0]
      mem_ds.WriteRaster(
          0,
          0,
          base_ds.RasterXSize,
          base_ds.RasterYSize,
          arr.tobytes(),
          buf_type=dt,
      )
      return mem_ds, dt_size

  def _build_direct_io_variant(
      self,
      filename: str,
      src_ds: gdal.Dataset,
      dt: int,
      variant: int,
      option: str,
      use_vsimem: bool,
  ) -> None:
    """Writes one layout variant for DirectIO/VirtualMemIO tests."""
    invert_opt = ['ENDIANNESS=INVERTED'] if dt != gdal.GDT_Byte else []
    tiled_opts = ['TILED=YES', 'BLOCKXSIZE=32', 'BLOCKYSIZE=16']
    if variant == 0:
      with self.driver.CreateCopy(filename, src_ds, options=invert_opt):
        pass
    elif variant == 1:
      with self.driver.CreateCopy(
          filename, src_ds, options=['INTERLEAVE=BAND']
      ):
        pass
    elif variant == 2:
      if option == 'GTIFF_VIRTUAL_MEM_IO' and use_vsimem:
        with gdal.Translate(
            filename,
            src_ds,
            bandList=[1, 2, 3],
            creationOptions=tiled_opts + invert_opt,
        ):
          pass
      else:
        with self.driver.CreateCopy(
            filename, src_ds, options=tiled_opts + invert_opt
        ):
          pass
    elif variant == 3:
      with self.driver.CreateCopy(
          filename, src_ds, options=tiled_opts + ['INTERLEAVE=BAND']
      ):
        pass
    elif variant == 4:
      with self.driver.Create(
          filename, 165, 150, 4, dt, options=['SPARSE_OK=YES']
      ):
        pass
    elif variant == 5:
      with self.driver.Create(
          filename,
          165,
          150,
          4,
          dt,
          options=['SPARSE_OK=YES', 'INTERLEAVE=BAND'],
      ):
        pass
    elif variant == 6:
      with self.driver.Create(
          filename, 165, 150, 4, dt, options=['SPARSE_OK=YES'] + tiled_opts
      ):
        pass
    else:
      with self.driver.Create(
          filename,
          165,
          150,
          4,
          dt,
          options=['SPARSE_OK=YES', 'INTERLEAVE=BAND'] + tiled_opts,
      ):
        pass

  def _truncate_tiff_vsi(self, filename: str, dt_size: int) -> None:
    """Truncates the trailing block bytes of a test TIFF file."""
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      nbands = ds.RasterCount
      nxsize = ds.RasterXSize
      nysize = ds.RasterYSize
      nblockx, nblocky = ds.GetRasterBand(1).GetBlockSize()
      band_interleaved = (
          ds.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE') == 'BAND'
      )
    padding = 0
    if nblockx < nxsize:
      if (nysize % nblocky) != 0:
        padding = ((nxsize + nblockx - 1) // nblockx * nblockx) * (
            nblocky - (nysize % nblocky)
        )
      if (nxsize % nblockx) != 0:
        padding += nblockx - (nxsize % nblockx)
      padding *= dt_size
      if not band_interleaved:
        padding *= nbands
    to_remove = 1 if band_interleaved else 1 + (nbands - 1) * dt_size
    size = gdal.VSIStatL(filename).size
    with gdal.VSIFile(filename, 'rb') as f:
      data = f.read(size)
    with gdal.VSIFile(filename, 'wb') as f:
      f.write(data[: len(data) - padding - to_remove])

  def _read_io_snapshots(
      self, ds: gdal.Dataset, dt_size: int, include_extra: bool
  ) -> dict[str, bytes | None]:
    """Reads all subwindow/resampling/interleaving combinations."""
    xoff = ds.RasterXSize // 4
    yoff = ds.RasterYSize // 4
    xsize = ds.RasterXSize // 2
    ysize = ds.RasterYSize // 2
    nbands = ds.RasterCount
    sizeof_float = 4
    band1 = ds.GetRasterBand(1)
    res: dict[str, bytes | None] = {
        'b1_sub': band1.ReadRaster(xoff, yoff, xsize, ysize),
        'b1_whole': band1.ReadRaster(),
        'b1_down': band1.ReadRaster(
            xoff, yoff, xsize, ysize, buf_xsize=xsize // 2, buf_ysize=ysize // 2
        ),
        'b1_bilinear': band1.ReadRaster(
            xoff,
            yoff,
            xsize,
            ysize,
            buf_xsize=xsize // 2,
            buf_ysize=ysize // 2,
            resample_alg=gdal.GRIORA_Bilinear,
        ),
        'b1_up': band1.ReadRaster(
            xoff,
            yoff,
            xsize,
            ysize,
            buf_xsize=nbands * xsize,
            buf_ysize=nbands * ysize,
        ),
        'b1_spacing': band1.ReadRaster(
            xoff, yoff, xsize, ysize, buf_pixel_space=nbands * dt_size
        ),
        'b1_f32': band1.ReadRaster(
            xoff, yoff, xsize, ysize, buf_type=gdal.GDT_Float32
        ),
        'ds_sub': ds.ReadRaster(xoff, yoff, xsize, ysize),
        'ds_whole': ds.ReadRaster(),
        'ds_pix_whole': ds.ReadRaster(
            buf_pixel_space=nbands * dt_size, buf_band_space=dt_size
        ),
        'ds_br_down': ds.ReadRaster(
            ds.RasterXSize - 2,
            ds.RasterYSize - 1,
            2,
            1,
            buf_xsize=1,
            buf_ysize=1,
            buf_pixel_space=nbands * dt_size,
            buf_band_space=dt_size,
        ),
    }
    if include_extra:
      res.update({
          'ds_down': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_xsize=xsize // 2,
              buf_ysize=ysize // 2,
          ),
          'ds_down_interleaved': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_xsize=xsize // 2,
              buf_ysize=ysize // 2,
              buf_pixel_space=nbands * dt_size,
              buf_band_space=dt_size,
          ),
          'ds_down_bilinear': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_xsize=xsize // 2,
              buf_ysize=ysize // 2,
              resample_alg=gdal.GRIORA_Bilinear,
          ),
          'ds_up': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_xsize=4 * xsize,
              buf_ysize=4 * ysize,
          ),
          'ds_down_x_up_y': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_xsize=xsize // 2,
              buf_ysize=32 * ysize,
          ),
          'ds_unordered': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              band_list=[nbands - i for i in range(nbands)],
          ),
          'ds_pix': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_pixel_space=nbands * dt_size,
              buf_band_space=dt_size,
          ),
          'ds_m1_pix': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              band_list=[i + 1 for i in range(nbands - 1)],
              buf_pixel_space=nbands * dt_size,
              buf_band_space=dt_size,
          ),
          'ds_f32': ds.ReadRaster(
              xoff, yoff, xsize, ysize, buf_type=gdal.GDT_Float32
          ),
          'ds_f32_pix': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_type=gdal.GDT_Float32,
              buf_pixel_space=nbands * sizeof_float,
              buf_band_space=sizeof_float,
          ),
          'ds_custom_2x': ds.ReadRaster(
              xoff,
              yoff,
              xsize,
              ysize,
              buf_pixel_space=2 * nbands * dt_size,
              buf_band_space=dt_size,
          ),
      })
      if nbands == 3:
        res['ds_custom_4'] = ds.ReadRaster(
            xoff,
            yoff,
            xsize,
            ysize,
            buf_pixel_space=4 * dt_size,
            buf_band_space=dt_size,
        )
    return res

  def _extract_strided_view(
      self,
      data: bytes | None,
      ysize: int,
      xsize: int,
      slot_bands: int,
      active_bands: int,
      dt_size: int,
  ) -> np.ndarray:
    if data is None:
      raise ValueError('Expected non-None strided raster buffer.')
    total = ysize * xsize * slot_bands * dt_size
    min_expected = total - (slot_bands - active_bands) * dt_size
    if not (min_expected <= len(data) <= total):
      raise ValueError(
          f'Unexpected strided buffer length {len(data)}; '
          f'expected [{min_expected}, {total}].'
      )
    arr = np.empty(total, dtype=np.uint8)
    arr[: len(data)] = np.frombuffer(data, dtype=np.uint8)
    return arr.reshape(ysize, xsize, slot_bands, dt_size)[
        :, :, :active_bands, :
    ]

  # Upstream: test_tiff_direct_and_virtual_mem_io
  @parameterized.named_parameters(
      dict(testcase_name='byte', dt=gdal.GDT_Byte, truncated=False),
      dict(testcase_name='byte_trunc', dt=gdal.GDT_Byte, truncated=True),
      dict(testcase_name='int16', dt=gdal.GDT_Int16, truncated=False),
      dict(testcase_name='int16_trunc', dt=gdal.GDT_Int16, truncated=True),
      dict(testcase_name='cint16', dt=gdal.GDT_CInt16, truncated=False),
      dict(testcase_name='cint16_trunc', dt=gdal.GDT_CInt16, truncated=True),
  )
  def test_tiff_direct_and_virtual_mem_io(
      self, dt: int, truncated: bool
  ) -> None:
    vsi_dir = self._vsimem_dir('direct_io')
    tmp_dir = str(pathlib.Path(self.create_tempdir().full_path))
    dt_sizes = {gdal.GDT_Byte: 1, gdal.GDT_Int16: 2}
    with self._create_direct_io_source(dt)[0] as src_ds:
      dt_size = dt_sizes.get(dt, 4)
      if truncated:
        nitermax = 4
        combos = (
            ('GTIFF_DIRECT_IO', vsi_dir, True),
            ('GTIFF_VIRTUAL_MEM_IO', vsi_dir, True),
        )
      else:
        nitermax = 8
        combos = (
            ('GTIFF_DIRECT_IO', vsi_dir, True),
            ('GTIFF_VIRTUAL_MEM_IO', vsi_dir, True),
            ('GTIFF_VIRTUAL_MEM_IO', tmp_dir, False),
        )

      for option, prefix, use_vsimem in combos:
        if dt == gdal.GDT_CInt16:
          niter = 3
        elif not use_vsimem:
          niter = 4
        else:
          niter = nitermax
        for variant in range(niter):
          filename = f'{prefix}/io_{option}_{variant}.tif'
          self._build_direct_io_variant(
              filename, src_ds, dt, variant, option, use_vsimem
          )
          if truncated:
            self._truncate_tiff_vsi(filename, dt_size)

          if truncated:
            with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
              with self._open_readonly(filename) as ref:
                band_interleaved = (
                    ref.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE')
                    == 'BAND'
                )
                nbands = ref.RasterCount
                xsize = ref.RasterXSize // 2
                ysize = ref.RasterYSize // 2
                ref_snap = self._read_io_snapshots(ref, dt_size, False)

              with gdal.config_option(option, 'YES'):
                with self._open_readonly(filename) as got:
                  got_snap = self._read_io_snapshots(got, dt_size, False)
          else:
            band_interleaved = False
            with self._open_readonly(filename) as ref:
              nbands = ref.RasterCount
              xsize = ref.RasterXSize // 2
              ysize = ref.RasterYSize // 2
              ref_snap = self._read_io_snapshots(ref, dt_size, True)

            with gdal.config_option(option, 'YES'):
              with self._open_readonly(filename) as got:
                got_snap = self._read_io_snapshots(got, dt_size, True)

          gdal.Unlink(filename)
          with self.subTest(option=option, vsimem=use_vsimem, variant=variant):
            for key in ('b1_sub', 'b1_down', 'b1_up', 'b1_f32'):
              if not truncated:
                self.assertIsNotNone(ref_snap[key], msg=key)
              self.assertEqual(got_snap[key], ref_snap[key], msg=key)
            if truncated and not band_interleaved:
              self.assertIsNone(got_snap['b1_whole'])
            else:
              if not truncated:
                self.assertIsNotNone(ref_snap['b1_whole'], msg='b1_whole')
              self.assertEqual(got_snap['b1_whole'], ref_snap['b1_whole'])

            ref_sp = self._extract_strided_view(
                ref_snap['b1_spacing'], ysize, xsize, nbands, 1, dt_size
            )
            got_sp = self._extract_strided_view(
                got_snap['b1_spacing'], ysize, xsize, nbands, 1, dt_size
            )
            np.testing.assert_array_equal(got_sp, ref_sp)

            if truncated:
              self.assertIsNone(got_snap['ds_whole'])
              self.assertIsNone(got_snap['ds_pix_whole'])
              self.assertIsNone(got_snap['ds_br_down'])
              continue

            for key in (
                'b1_bilinear',
                'ds_sub',
                'ds_whole',
                'ds_down',
                'ds_down_interleaved',
                'ds_down_bilinear',
                'ds_up',
                'ds_down_x_up_y',
                'ds_unordered',
                'ds_pix',
                'ds_pix_whole',
                'ds_f32',
                'ds_f32_pix',
            ):
              self.assertIsNotNone(ref_snap[key], msg=key)
              self.assertEqual(got_snap[key], ref_snap[key], msg=key)

            ref_m1 = self._extract_strided_view(
                ref_snap['ds_m1_pix'],
                ysize,
                xsize,
                nbands,
                nbands - 1,
                dt_size,
            )
            got_m1 = self._extract_strided_view(
                got_snap['ds_m1_pix'],
                ysize,
                xsize,
                nbands,
                nbands - 1,
                dt_size,
            )
            np.testing.assert_array_equal(got_m1, ref_m1)

            ref_2x = self._extract_strided_view(
                ref_snap['ds_custom_2x'],
                ysize,
                xsize,
                2 * nbands,
                nbands,
                dt_size,
            )
            got_2x = self._extract_strided_view(
                got_snap['ds_custom_2x'],
                ysize,
                xsize,
                2 * nbands,
                nbands,
                dt_size,
            )
            np.testing.assert_array_equal(got_2x, ref_2x)

            if nbands == 3:
              ref_4 = self._extract_strided_view(
                  ref_snap['ds_custom_4'], ysize, xsize, 4, 3, dt_size
              )
              got_4 = self._extract_strided_view(
                  got_snap['ds_custom_4'], ysize, xsize, 4, 3, dt_size
              )
              np.testing.assert_array_equal(got_4, ref_4)

  # Upstream: test_tiff_read_md1..md11
  @parameterized.named_parameters(
      dict(
          testcase_name='md1_digitalglobe_imd_rpb',
          tif_rel='md_dg.tif',
          sidecars=('md_dg.IMD', 'md_dg.RPB'),
          expected_files=3,
          expected_domains=6,
          check_cloud=True,
          expected_dt='2010-04-01 12:00:00',
      ),
      dict(
          testcase_name='md2_digitalglobe_xml',
          tif_rel='md_dg_2.tif',
          sidecars=('md_dg_2.XML',),
          expected_files=2,
          expected_domains=6,
          check_cloud=True,
          expected_dt='2011-05-01 13:00:00',
      ),
      dict(
          testcase_name='md3_geoeye',
          tif_rel='md_ge_rgb_0010000.tif',
          sidecars=('md_ge_metadata.txt', 'md_ge_rgb_0010000_rpc.txt'),
          expected_files=3,
          expected_domains=6,
          check_cloud=True,
          expected_dt='2012-06-01 14:00:00',
      ),
      dict(
          testcase_name='md4_orbview',
          tif_rel='md_ov.tif',
          sidecars=('md_ov.pvl', 'md_ov_rpc.txt'),
          expected_files=3,
          expected_domains=6,
          check_cloud=True,
          expected_dt='2013-07-01 15:00:00',
      ),
      dict(
          testcase_name='md5_resurs_dk1',
          tif_rel='md_rdk1.tif',
          sidecars=('md_rdk1.xml',),
          expected_files=2,
          expected_domains=5,
          check_cloud=True,
          expected_dt='2014-08-01 16:00:00',
      ),
      dict(
          testcase_name='md6_landsat',
          tif_rel='md_ls_b1.tif',
          sidecars=('md_ls_MTL.txt',),
          expected_files=2,
          expected_domains=5,
          check_cloud=True,
          expected_dt='2015-09-01 17:00:00',
      ),
      dict(
          testcase_name='md7_spot',
          tif_rel='spot/md_spot.tif',
          sidecars=('spot/METADATA.DIM',),
          expected_files=2,
          expected_domains=5,
          check_cloud=True,
          expected_dt='2001-03-01 00:00:00',
      ),
      dict(
          testcase_name='md8_rapideye',
          tif_rel='md_re.tif',
          sidecars=('md_re_metadata.xml',),
          expected_files=2,
          expected_domains=5,
          check_cloud=True,
          expected_dt='2010-02-01 12:00:00',
      ),
      dict(
          testcase_name='md9_alos',
          tif_rel='alos/IMG-md_alos.tif',
          sidecars=('alos/RPC-md_alos.txt', 'alos/summary.txt'),
          expected_files=3,
          expected_domains=6,
          check_cloud=False,
          expected_dt='2010-07-01 00:00:00',
      ),
      dict(
          testcase_name='md10_eros',
          tif_rel='md_eros.tif',
          sidecars=('md_eros.pass', 'md_eros.rpc'),
          expected_files=3,
          expected_domains=6,
          check_cloud=True,
          expected_dt='2013-04-01 11:00:00',
      ),
      dict(
          testcase_name='md11_kompsat',
          tif_rel='md_kompsat.tif',
          sidecars=('md_kompsat.rpc', 'md_kompsat.txt'),
          expected_files=3,
          expected_domains=6,
          check_cloud=True,
          expected_dt='2007-05-01 07:00:00',
      ),
  )
  def test_satellite_metadata_readers(
      self,
      tif_rel: str,
      sidecars: Sequence[str],
      expected_files: int,
      expected_domains: int,
      check_cloud: bool,
      expected_dt: str,
  ) -> None:
    case_dir = pathlib.Path(self.create_tempdir().full_path)
    tif_path = case_dir / tif_rel
    tif_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(self._test_path('rgbsmall.tif'), tif_path)
    for sidecar in sidecars:
      dst = case_dir / sidecar
      dst.parent.mkdir(parents=True, exist_ok=True)
      shutil.copyfile(self._test_path(sidecar), dst)

    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with self._open_readonly(str(tif_path)) as ds:
        self.assertLen(ds.GetFileList(), expected_files)
        self.assertLen(ds.GetMetadataDomainList(), expected_domains)
        imagery_md = ds.GetMetadata('IMAGERY')
        self.assertIn('SATELLITEID', imagery_md)
        if check_cloud:
          self.assertIn('CLOUDCOVER', imagery_md)
        self.assertEqual(imagery_md['ACQUISITIONDATETIME'], expected_dt)
    self.assertFalse(pathlib.Path(f'{tif_path}.aux.xml').exists())

  # Upstream: test_tiff_read_md1 (IMD order) & test_tiff_read_md9 (ALOS RPC)
  def test_imd_unsorted_and_alos_rpc_metadata(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    dg_tif = tmp_dir / 'md_dg.tif'
    shutil.copyfile(self._test_path('rgbsmall.tif'), dg_tif)
    shutil.copyfile(self._test_path('md_dg.IMD'), tmp_dir / 'md_dg.IMD')
    shutil.copyfile(self._test_path('md_dg.RPB'), tmp_dir / 'md_dg.RPB')
    with self._open_readonly(str(dg_tif)) as ds:
      imd_list = ds.GetMetadata_List('IMD')
      self.assertEqual(imd_list[0], 'version="24.06"')

    alos_dir = tmp_dir / 'alos'
    alos_dir.mkdir()
    alos_tif = alos_dir / 'IMG-md_alos.tif'
    shutil.copyfile(self._test_path('rgbsmall.tif'), alos_tif)
    shutil.copyfile(
        self._test_path('alos/RPC-md_alos.txt'), alos_dir / 'RPC-md_alos.txt'
    )
    shutil.copyfile(
        self._test_path('alos/summary.txt'), alos_dir / 'summary.txt'
    )
    with self._open_readonly(str(alos_tif)) as ds:
      rpc = ds.GetMetadata('RPC')
      expected_rpc_prefixes = {
          'HEIGHT_OFF': '+3000',
          'HEIGHT_SCALE': '+3158',
          'LAT_OFF': '+55.8151',
          'LAT_SCALE': '+00.4400',
          'LINE_OFF': '004000',
          'LINE_SCALE': '004129',
          'LONG_OFF': '+032.0758',
          'LONG_SCALE': '+000.7304',
          'SAMP_OFF': '03639',
          'SAMP_SCALE': '03699',
      }
      for key, val in expected_rpc_prefixes.items():
        self.assertEqual(rpc[key], val, msg=key)
      self.assertTrue(
          rpc['LINE_DEN_COEFF'].startswith('+1.000000E+0 -2.395249E-3')
      )
      self.assertTrue(
          rpc['LINE_NUM_COEFF'].startswith('-3.910052E-4 -3.183540E-1')
      )
      self.assertTrue(
          rpc['SAMP_DEN_COEFF'].startswith('+1.000000E+0 -2.395249E-3')
      )
      self.assertTrue(
          rpc['SAMP_NUM_COEFF'].startswith('+7.854784E-3 +1.190927E+0')
      )

  # Upstream: test_tiff_read_non_conformant_imd
  def test_tiff_read_non_conformant_imd(self) -> None:
    vsi_dir = self._vsimem_dir('non_conformant_imd')
    gdal.FileFromMemBuffer(
        f'{vsi_dir}/test.imd',
        "BEGIN_GROUP = foo\n\tkey = value with space ' not quoted;\n"
        '\tkey2 = another one ;\r\nEND_GROUP\nEND\n',
    )
    self._copy_to_vsimem('byte.tif', f'{vsi_dir}/test.tif')
    with self._open_readonly(f'{vsi_dir}/test.tif') as ds:
      self.assertEqual(
          ds.GetMetadata('IMD'),
          {
              'foo.key': "value with space ' not quoted",
              'foo.key2': 'another one',
          },
      )

  # Upstream: test_tiff_read_md12
  def test_tiff_read_md12_dimap(self) -> None:
    dimap_tif = self._test_path('dimap2/single_component/IMG_foo_R2C1.TIF')
    dim_xml = self._test_path('dimap2/single_component/DIM_foo.XML')
    rpc_xml = self._test_path('dimap2/single_component/RPC_foo.XML')
    with self._open_readonly(dimap_tif) as ds:
      self.assertCountEqual(ds.GetFileList(), [dimap_tif, dim_xml, rpc_xml])
      self.assertLen(ds.GetMetadataDomainList(), 6)
      imagery_md = ds.GetMetadata('IMAGERY')
      self.assertIn('SATELLITEID', imagery_md)
      self.assertIn('CLOUDCOVER', imagery_md)
      self.assertEqual(imagery_md['ACQUISITIONDATETIME'], '2016-06-17 12:34:56')
      self.assertEqual(ds.GetMetadata('RPC')['LINE_OFF'], '-11')

    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tmp_tif = str(tmp_dir / 'IMG_foo_temp.TIF')
    shutil.copyfile(dimap_tif, tmp_tif)
    shutil.copyfile(dim_xml, tmp_dir / 'DIM_foo.XML')
    shutil.copyfile(rpc_xml, tmp_dir / 'RPC_foo.XML')
    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with self._open_readonly(tmp_tif) as ds:
        self.assertEqual(ds.GetFileList(), [tmp_tif])
    self.assertFalse(pathlib.Path(f'{tmp_tif}.aux.xml').exists())

  # Upstream: test_tiff_read_empty_nodata_tag
  def test_tiff_read_empty_nodata_tag(self) -> None:
    vsi_dir = self._vsimem_dir('empty_nodata')
    filepath = f'{vsi_dir}/empty_nodata.tif'
    entries = _minimal_1x1_entries(
        strip_offset=_TailStripOffsets(),
        extra_entries=((42113, _TIFF_ASCII, 1, b'\x00'),),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00')
    )
    with self._open_readonly(filepath) as ds:
      self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())

  # Upstream test_tiff_read_strace_check omitted: Blaze Forge sandbox does not
  # ship strace or allow ptrace across processes.

  # Upstream: test_tiff_read_readdir_limit_on_open
  def test_tiff_read_readdir_limit_on_open(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tif_path = tmp_dir / 'md_kompsat.tif'
    shutil.copyfile(self._test_path('rgbsmall.tif'), tif_path)
    shutil.copyfile(
        self._test_path('md_kompsat.rpc'), tmp_dir / 'md_kompsat.rpc'
    )
    shutil.copyfile(
        self._test_path('md_kompsat.txt'), tmp_dir / 'md_kompsat.txt'
    )
    with gdal.config_option('GDAL_READDIR_LIMIT_ON_OPEN', '1'):
      with gdal.OpenEx(str(tif_path), gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self.assertLen(ds.GetFileList(), 3)

  # Upstream: test_tiff_read_scanline_more_than_2GB
  def test_tiff_read_scanline_more_than_2gb(self) -> None:
    vsi_dir = self._vsimem_dir('scanline_gt_2gb')
    filepath = f'{vsi_dir}/scanline_more_than_2GB.tif'
    entries = _minimal_1x1_entries(
        width=2147483647,
        width_type=_TIFF_LONG,
        bits_per_sample=struct.pack('<HH', 8, 8),
        bits_count=2,
        compression=2,
        strip_offset=_TailStripOffsets(),
        strip_byte_count=2,
        samples_per_pixel=2,
        sample_format=struct.pack('<HH', 1, 1),
        sample_format_count=2,
        extra_entries=((338, _TIFF_SHORT, 1, 0),),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00\x00')
    )
    # Google3 executes on 64-bit targets where >2 GB scanlines are openable.
    with self._open_readonly(filepath) as ds:
      self.assertEqual(ds.RasterXSize, 2147483647)

  # Upstream: test_tiff_read_wrong_number_extrasamples
  def test_tiff_read_wrong_number_extrasamples(self) -> None:
    filepath = self._test_path('6band_wrong_number_extrasamples.tif')
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        self.assertIn('Wrong number of ExtraSamples', gdal.GetLastErrorMsg())
        self.assertEqual(
            ds.GetRasterBand(6).GetRasterColorInterpretation(),
            gdal.GCI_AlphaBand,
        )

  # Upstream: test_tiff_read_one_strip_no_bytecount
  def test_tiff_read_one_strip_no_bytecount(self) -> None:
    filepath = self._test_path('one_strip_nobytecount.tif')
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        arr = self._assert_band(ds.GetRasterBand(1), 1, (2001, 1), np.uint8)
        self.assertGreater(int(arr.max()), 0)

  def _setup_georef_sources_dir(
      self,
      vsi_dir: str,
      base_tif: str,
      strip_internal_georef: bool,
      copy_pam: bool,
      copy_worldfile: bool,
      copy_tabfile: bool,
  ) -> str:
    tif_path = f'{vsi_dir}/sample.tif'
    if strip_internal_georef:
      with gdal.Translate(
          tif_path, base_tif, creationOptions=['PROFILE=BASELINE']
      ):
        pass
    else:
      with gdal.Translate(tif_path, base_tif):
        pass
    if copy_pam:
      gdal.FileFromMemBuffer(
          f'{tif_path}.aux.xml',
          '<PAMDataset>\n'
          '  <SRS>LOCAL_CS["PAM"]</SRS>\n'
          '  <GeoTransform>1,2,3,4,5,6</GeoTransform>\n'
          '</PAMDataset>\n',
      )
    if copy_worldfile:
      gdal.FileFromMemBuffer(
          f'{vsi_dir}/sample.tfw', '1,\n0,\n0,\n-1,\n100,\n200\n'
      )
    if copy_tabfile:
      gdal.FileFromMemBuffer(
          f'{vsi_dir}/sample.tab',
          '!table\n!version 300\n!charset WindowsLatin1\n\n'
          'Definition Table\n  File "HP.TIF"\n  Type "RASTER"\n'
          '  (400000,1200000) (0,4000) Label "Pt 1",\n'
          '  (500000,1200000) (4000,4000) Label "Pt 2",\n'
          '  (500000,1300000) (4000,0) Label "Pt 3",\n'
          '  (400000,1300000) (0,0) Label "Pt 4"\n'
          '  CoordSys Earth Projection 8, 79, "m", -2, 49, 0.9996012717, '
          '400000, -100000\n  Units "m"\n',
      )
    return tif_path

  # Upstream: test_tiff_read_nogeoref
  @parameterized.named_parameters(
      dict(
          testcase_name='default_pam_tfw',
          cfg=None,
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='default_all',
          cfg=None,
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='default_tfw_tab',
          cfg=None,
          pam=False,
          tfw=True,
          tab=True,
          srs='_1936',
          gt=(400000.0, 25.0, 0.0, 1300000.0, 0.0, -25.0),
      ),
      dict(
          testcase_name='default_pam_only',
          cfg=None,
          pam=True,
          tfw=False,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='default_tfw_only',
          cfg=None,
          pam=False,
          tfw=True,
          tab=False,
          srs='',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='default_none',
          cfg=None,
          pam=False,
          tfw=False,
          tab=False,
          srs='',
          gt=EMPTY_GEOTRANSFORM,
      ),
      dict(
          testcase_name='internal',
          cfg='INTERNAL',
          pam=True,
          tfw=True,
          tab=False,
          srs='',
          gt=EMPTY_GEOTRANSFORM,
      ),
      dict(
          testcase_name='internal_pam',
          cfg='INTERNAL,PAM',
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='internal_worldfile',
          cfg='INTERNAL,WORLDFILE',
          pam=True,
          tfw=True,
          tab=True,
          srs='',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='internal_pam_worldfile',
          cfg='INTERNAL,PAM,WORLDFILE',
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='internal_worldfile_pam',
          cfg='INTERNAL,WORLDFILE,PAM',
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='worldfile_pam_internal_only_tab',
          cfg='WORLDFILE,PAM,INTERNAL',
          pam=False,
          tfw=False,
          tab=True,
          srs='',
          gt=EMPTY_GEOTRANSFORM,
      ),
      dict(
          testcase_name='pam_worldfile_internal_only_tab',
          cfg='PAM,WORLDFILE,INTERNAL',
          pam=False,
          tfw=False,
          tab=True,
          srs='',
          gt=EMPTY_GEOTRANSFORM,
      ),
      dict(
          testcase_name='tabfile_worldfile_internal',
          cfg='TABFILE,WORLDFILE,INTERNAL',
          pam=True,
          tfw=True,
          tab=True,
          srs='_1936',
          gt=(400000.0, 25.0, 0.0, 1300000.0, 0.0, -25.0),
      ),
      dict(
          testcase_name='pam',
          cfg='PAM',
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='pam_worldfile',
          cfg='PAM,WORLDFILE',
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='worldfile',
          cfg='WORLDFILE',
          pam=True,
          tfw=True,
          tab=False,
          srs='',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='worldfile_pam',
          cfg='WORLDFILE,PAM',
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='worldfile_internal',
          cfg='WORLDFILE,INTERNAL',
          pam=True,
          tfw=True,
          tab=False,
          srs='',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='worldfile_pam_internal',
          cfg='WORLDFILE,PAM,INTERNAL',
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='worldfile_internal_pam',
          cfg='WORLDFILE,INTERNAL,PAM',
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='none',
          cfg='NONE',
          pam=True,
          tfw=True,
          tab=False,
          srs='',
          gt=EMPTY_GEOTRANSFORM,
      ),
  )
  def test_tiff_read_nogeoref_sources(
      self,
      cfg: str | None,
      pam: bool,
      tfw: bool,
      tab: bool,
      srs: str,
      gt: tuple[float, ...],
  ) -> None:
    vsi_dir = self._vsimem_dir('nogeoref')
    tif_path = self._setup_georef_sources_dir(
        vsi_dir, self._test_path('byte.tif'), True, pam, tfw, tab
    )
    for order in ('gt_first', 'srs_first'):
      with self.subTest(order=order):
        with gdal.config_options(
            {'GDAL_GEOREF_SOURCES': cfg, 'GDAL_PAM_ENABLED': 'YES'}
        ):
          with gdal.OpenEx(tif_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
            if order == 'gt_first':
              got_gt = ds.GetGeoTransform()
              got_srs = ds.GetProjectionRef()
            else:
              got_srs = ds.GetProjectionRef()
              got_gt = ds.GetGeoTransform()
        self.assertEqual(got_gt, gt)
        if not srs:
          self.assertEqual(got_srs, '')
        else:
          self.assertIn(srs, got_srs)

  # Upstream: test_tiff_read_inconsistent_georef
  @parameterized.named_parameters(
      dict(
          testcase_name='default_all',
          cfg=None,
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='default_tfw_tab',
          cfg=None,
          pam=False,
          tfw=True,
          tab=True,
          srs='26711',
          gt=BYTE_GEOTRANSFORM,
      ),
      dict(
          testcase_name='default_tab',
          cfg=None,
          pam=False,
          tfw=False,
          tab=True,
          srs='26711',
          gt=BYTE_GEOTRANSFORM,
      ),
      dict(
          testcase_name='default_tfw',
          cfg=None,
          pam=False,
          tfw=True,
          tab=False,
          srs='26711',
          gt=BYTE_GEOTRANSFORM,
      ),
      dict(
          testcase_name='default_none',
          cfg=None,
          pam=False,
          tfw=False,
          tab=False,
          srs='26711',
          gt=BYTE_GEOTRANSFORM,
      ),
      dict(
          testcase_name='default_pam_tab',
          cfg=None,
          pam=True,
          tfw=False,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='default_pam_tfw',
          cfg=None,
          pam=True,
          tfw=True,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='default_pam_only',
          cfg=None,
          pam=True,
          tfw=False,
          tab=False,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='internal',
          cfg='INTERNAL',
          pam=True,
          tfw=True,
          tab=True,
          srs='26711',
          gt=BYTE_GEOTRANSFORM,
      ),
      dict(
          testcase_name='pam',
          cfg='PAM',
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='pam_tabfile',
          cfg='PAM,TABFILE',
          pam=True,
          tfw=True,
          tab=True,
          srs='LOCAL_CS["PAM"',
          gt=(1.0, 2.0, 3.0, 4.0, 5.0, 6.0),
      ),
      dict(
          testcase_name='worldfile',
          cfg='WORLDFILE',
          pam=True,
          tfw=True,
          tab=True,
          srs='',
          gt=(99.5, 1.0, 0.0, 200.5, 0.0, -1.0),
      ),
      dict(
          testcase_name='tabfile',
          cfg='TABFILE',
          pam=True,
          tfw=True,
          tab=True,
          srs='_1936',
          gt=(400000.0, 25.0, 0.0, 1300000.0, 0.0, -25.0),
      ),
      dict(
          testcase_name='tabfile_pam',
          cfg='TABFILE,PAM',
          pam=True,
          tfw=True,
          tab=True,
          srs='_1936',
          gt=(400000.0, 25.0, 0.0, 1300000.0, 0.0, -25.0),
      ),
  )
  def test_tiff_read_inconsistent_georef(
      self,
      cfg: str | None,
      pam: bool,
      tfw: bool,
      tab: bool,
      srs: str,
      gt: tuple[float, ...],
  ) -> None:
    vsi_dir = self._vsimem_dir('inconsistent_georef')
    tif_path = self._setup_georef_sources_dir(
        vsi_dir, self._test_path('byte.tif'), False, pam, tfw, tab
    )
    for order in ('gt_first', 'srs_first'):
      with self.subTest(order=order):
        with gdal.config_options(
            {'GDAL_GEOREF_SOURCES': cfg, 'GDAL_PAM_ENABLED': 'YES'}
        ):
          with gdal.OpenEx(tif_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
            if order == 'gt_first':
              got_gt = ds.GetGeoTransform()
              got_srs = ds.GetProjectionRef()
            else:
              got_srs = ds.GetProjectionRef()
              got_gt = ds.GetGeoTransform()
        self.assertEqual(got_gt, gt)
        if not srs:
          self.assertEqual(got_srs, '')
        else:
          self.assertIn(srs, got_srs)

  # Upstream: test_tiff_read_gcp_internal_and_auxxml
  @parameterized.named_parameters(
      dict(testcase_name='default_pam', cfg=None, pam=True, srs='PAM', gcps=1),
      dict(
          testcase_name='default_nopam', cfg=None, pam=False, srs='4326', gcps=2
      ),
      dict(
          testcase_name='internal_pam',
          cfg='INTERNAL',
          pam=True,
          srs='4326',
          gcps=2,
      ),
      dict(
          testcase_name='internal_nopam',
          cfg='INTERNAL',
          pam=False,
          srs='4326',
          gcps=2,
      ),
      dict(
          testcase_name='internal_pam_list',
          cfg='INTERNAL,PAM',
          pam=True,
          srs='4326',
          gcps=2,
      ),
      dict(
          testcase_name='internal_pam_list_nopam',
          cfg='INTERNAL,PAM',
          pam=False,
          srs='4326',
          gcps=2,
      ),
      dict(testcase_name='pam', cfg='PAM', pam=True, srs='PAM', gcps=1),
      dict(testcase_name='pam_nopam', cfg='PAM', pam=False, srs='', gcps=0),
      dict(
          testcase_name='pam_internal',
          cfg='PAM,INTERNAL',
          pam=True,
          srs='PAM',
          gcps=1,
      ),
      dict(
          testcase_name='pam_internal_nopam',
          cfg='PAM,INTERNAL',
          pam=False,
          srs='4326',
          gcps=2,
      ),
  )
  def test_tiff_read_gcp_internal_and_auxxml(
      self, cfg: str | None, pam: bool, srs: str, gcps: int
  ) -> None:
    vsi_dir = self._vsimem_dir('gcp_sources')
    tif_path = f'{vsi_dir}/byte_gcp.tif'
    self._copy_to_vsimem('byte_gcp.tif', tif_path)
    if pam:
      gdal.FileFromMemBuffer(
          f'{tif_path}.aux.xml',
          '<PAMDataset>\n'
          '  <GCPList Projection="LOCAL_CS[&quot;PAM&quot;]">\n'
          '    <GCP Id="1" Pixel="0.0000" Line="0.0000" X="0" Y="0" />\n'
          '  </GCPList>\n'
          '</PAMDataset>\n',
      )
    open_options = [f'GEOREF_SOURCES={cfg}'] if cfg is not None else []
    for order in ('count_first', 'srs_first'):
      with self.subTest(order=order):
        with gdal.config_option('GDAL_PAM_ENABLED', 'YES'):
          with gdal.OpenEx(
              tif_path,
              gdal.OF_RASTER | gdal.OF_READONLY,
              open_options=open_options,
          ) as ds:
            if order == 'count_first':
              got_count = ds.GetGCPCount()
              got_srs = ds.GetGCPProjection()
            else:
              got_srs = ds.GetGCPProjection()
              got_count = ds.GetGCPCount()
        self.assertEqual(got_count, gcps)
        if not srs:
          self.assertEqual(got_srs, '')
        else:
          self.assertIn(srs, got_srs)

  # Upstream: test_tiff_read_aux
  def test_tiff_read_aux(self) -> None:
    aux_path = self._test_path('f2r23.aux')
    filepath = self._test_path('f2r23.tif')
    captured_msgs: list[str] = []

    def _capture(err_class: int, err_no: int, msg: str) -> None:
      del err_class, err_no
      captured_msgs.append(msg)

    gdal.ErrorReset()
    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with self._open_readonly(filepath) as ds:
        gdal.PushErrorHandler(_capture)
        try:
          file_list = ds.GetFileList()
        finally:
          gdal.PopErrorHandler()
        rat = self._require_band_rat(ds.GetRasterBand(1))
        self.assertEqual(rat.GetRowCount(), 218)
    self.assertEmpty(captured_msgs)
    self.assertCountEqual(file_list, [filepath, aux_path])

  # Upstream: test_tiff_read_one_band_from_two_bands
  def test_tiff_read_one_band_from_two_bands(self) -> None:
    vsi_dir = self._vsimem_dir('one_from_two')
    two_band = f'{vsi_dir}/two.tif'
    one_band = f'{vsi_dir}/one.tif'
    byte_tif = self._test_path('byte.tif')
    with gdal.Translate(two_band, byte_tif, bandList=[1, 1]):
      pass
    with gdal.Translate(one_band, two_band, bandList=[1]):
      pass
    with self._open_readonly(one_band) as ds:
      self.assertEqual(ds.RasterCount, 1)
      self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_jpeg_cloud_optimized
  @parameterized.named_parameters(
      dict(testcase_name='mode0', index=0),
      dict(testcase_name='mode1', index=1),
      dict(testcase_name='mode2', index=2),
      dict(testcase_name='mode3', index=3),
  )
  def test_tiff_read_jpeg_cloud_optimized(self, index: int) -> None:
    filepath = self._test_path(f'byte_ovr_jpeg_tablesmode{index}.tif')
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      arr = self._assert_band(band, 4743, (20, 20), np.uint8)
      self.assertGreater(int(arr.max()), 0)
      ovr = self._require_overview(band, 0)
      ovr_arr = self._assert_band(ovr, 1133, (10, 10), np.uint8)
      self.assertGreater(int(ovr_arr.max()), 0)

  # Upstream: test_tiff_read_corrupted_jpeg_cloud_optimized
  def test_tiff_read_corrupted_jpeg_cloud_optimized(self) -> None:
    filepath = self._test_path(
        'byte_ovr_jpeg_tablesmode_not_correctly_set_on_ovr.tif'
    )
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      self._assert_band(band, 4743, (20, 20), np.uint8)
      ovr = self._require_overview(band, 0)
      ovr_arr = self._assert_band(ovr, 1133, (10, 10), np.uint8)
      self.assertGreater(int(ovr_arr.max()), 0)

  # Upstream: test_tiff_read_ycbcr_lzw
  @parameterized.named_parameters(
      dict(
          testcase_name='11',
          filename='ycbcr_11_lzw.tif',
          expected=(13459, 12939, 12414),
      ),
      dict(
          testcase_name='12',
          filename='ycbcr_12_lzw.tif',
          expected=(13565, 13105, 12660),
      ),
      dict(
          testcase_name='21',
          filename='ycbcr_21_lzw.tif',
          expected=(13587, 13297, 12760),
      ),
      dict(
          testcase_name='22',
          filename='ycbcr_22_lzw.tif',
          expected=(13393, 13137, 12656),
      ),
      dict(
          testcase_name='41',
          filename='ycbcr_41_lzw.tif',
          expected=(13218, 12758, 12592),
      ),
      dict(
          testcase_name='42',
          filename='ycbcr_42_lzw.tif',
          expected=(13277, 12779, 12614),
      ),
      dict(
          testcase_name='42_opt',
          filename='ycbcr_42_lzw_optimized.tif',
          expected=(19918, 20120, 19087),
      ),
      dict(
          testcase_name='44',
          filename='ycbcr_44_lzw.tif',
          expected=(12994, 13229, 12149),
      ),
      dict(
          testcase_name='44_opt',
          filename='ycbcr_44_lzw_optimized.tif',
          expected=(19666, 19860, 18836),
      ),
  )
  def test_tiff_read_ycbcr_lzw_valid(
      self, filename: str, expected: tuple[int, int, int]
  ) -> None:
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      got = tuple(ds.GetRasterBand(b + 1).Checksum() for b in range(3))
      self.assertEqual(got, expected)
      arr = ds.ReadAsArray()
      if arr is None:
        self.fail('Expected non-None ReadAsArray() result')
      self.assertEqual(arr.shape, (3, ds.RasterYSize, ds.RasterXSize))
      self.assertEqual(arr.dtype, np.uint8)

  # Upstream: test_tiff_read_ycbcr_lzw (unsupported subsampling cases)
  @parameterized.named_parameters(
      dict(testcase_name='14', filename='ycbcr_14_lzw.tif'),
      dict(testcase_name='24', filename='ycbcr_24_lzw.tif'),
  )
  def test_tiff_read_ycbcr_lzw_unsupported(self, filename: str) -> None:
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      for band_idx in (1, 2, 3):
        with self.subTest(band=band_idx):
          with (
              gdal.quiet_errors(),
              self.assertRaisesRegex(
                  RuntimeError,
                  r'Sorry, can not handle image|TIFFReadRGBAStrip\(\) failed',
              ),
          ):
            ds.GetRasterBand(band_idx).Checksum()

  # Upstream: test_tiff_read_ycbcr_int12
  def test_tiff_read_ycbcr_int12(self) -> None:
    vsi_dir = self._vsimem_dir('ycbcr_int12')
    filepath = f'{vsi_dir}/int12_ycbcr_contig.tif'
    entries = _minimal_1x1_entries(
        bits_per_sample=struct.pack('<3H', 12, 12, 12),
        bits_count=3,
        photometric=6,
        strip_offset=_TailStripOffsets(),
        strip_byte_count=5,
        samples_per_pixel=3,
        sample_format=struct.pack('<3H', 1, 1, 1),
        sample_format_count=3,
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00' * 5)
    )
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, r'Cannot open TIFF file with'),
    ):
      self._open_readonly(filepath)

  # Upstream: test_tiff_read_unit_from_srs
  def test_tiff_read_unit_from_srs(self) -> None:
    vsi_dir = self._vsimem_dir('unit_from_srs')
    filepath = f'{vsi_dir}/unit_from_srs.tif'
    with self.driver.Create(filepath, 1, 1) as ds:
      srs = osr.SpatialReference()
      srs.SetFromUserInput('EPSG:4326+3855')
      ds.SetProjection(srs.ExportToWkt())
    with self._open_readonly(filepath) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetUnitType(), 'metre')

  # Upstream: test_tiff_read_arcgis93_geodataxform_gcp
  def test_tiff_read_arcgis93_geodataxform_gcp(self) -> None:
    self._test_path('arcgis93_geodataxform_gcp.tif.aux.xml')
    filepath = self._test_path('arcgis93_geodataxform_gcp.tif')
    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with self._open_readonly(filepath) as ds:
        self.assertIn('26712', ds.GetGCPProjection())
        self.assertEqual(ds.GetGCPCount(), 16)
        gcp = ds.GetGCPs()[0]
        self.assertAlmostEqual(gcp.GCPPixel, 565.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPLine, 11041.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPX, 500000.0, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPY, 4705078.79016612, delta=1e-5)
        self.assertAlmostEqual(gcp.GCPZ, 0.0, delta=1e-5)

  # Upstream: test_tiff_read_arcgis10_geodataxform_gcp_ignored
  def test_tiff_read_arcgis10_geodataxform_gcp_ignored(self) -> None:
    self._test_path('gtiff/esri_geodataxform_no_resolutionunit.tif.aux.xml')
    filepath = self._test_path('gtiff/esri_geodataxform_no_resolutionunit.tif')
    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with self._open_readonly(filepath) as ds:
        srs = self._require_srs(ds)
        self.assertEqual(srs.GetAuthorityCode(None), '3857')
        self.assertEqual(ds.GetGCPCount(), 0)
        self._assert_geotransform(
            ds,
            (
                -13714484.279678488,
                1.5302973005632299,
                0.0,
                6321896.159407652,
                0.0,
                -1.5346879055426712,
            ),
        )

  # Upstream: test_tiff_read_block_width_above_32bit, image_width_above_32bit,
  # huge_tile, unknown_compression, uint33, toomanyblocks,
  # toomanyblocks_separate
  @parameterized.named_parameters(
      dict(
          testcase_name='block_width_above_32bit',
          entries=(
              (256, _TIFF_SHORT, 1, 1),
              (257, _TIFF_SHORT, 1, 1),
              (258, _TIFF_SHORT, 1, 1),
              (259, _TIFF_SHORT, 1, 1),
              (262, _TIFF_SHORT, 1, 1),
              (277, _TIFF_SHORT, 1, 1),
              (284, _TIFF_SHORT, 1, 1),
              (322, _TIFF_LONG, 1, 0x80000000),
              (323, _TIFF_SHORT, 1, 1),
              (324, _TIFF_LONG, 1, 0),
              (325, _TIFF_LONG, 1, 0),
              (339, _TIFF_SHORT, 1, 1),
          ),
          err_regex=r'Too large block size: 2147483648 x 1',
      ),
      dict(
          testcase_name='image_width_above_32bit',
          entries=_minimal_1x1_entries(
              width=0x80000000,
              width_type=_TIFF_LONG,
              strip_offset=0,
              strip_byte_count=0,
          ),
          err_regex=r'Too large image size: 2147483648 x 1',
      ),
      dict(
          testcase_name='huge_tile',
          entries=(
              (256, _TIFF_SHORT, 1, 1),
              (257, _TIFF_SHORT, 1, 1),
              (258, _TIFF_SHORT, 1, 8),
              (259, _TIFF_SHORT, 1, 1),
              (262, _TIFF_SHORT, 1, 1),
              (277, _TIFF_SHORT, 1, 1),
              (284, _TIFF_SHORT, 1, 1),
              (322, _TIFF_LONG, 1, 0x80000000),
              (323, _TIFF_LONG, 1, 16),
              (324, _TIFF_LONG, 1, 0),
              (325, _TIFF_LONG, 1, 0),
              (339, _TIFF_SHORT, 1, 1),
          ),
          err_regex=r'Too large block size: 2147483648 x 16',
      ),
      dict(
          testcase_name='unknown_compression',
          entries=_minimal_1x1_entries(
              compression=65535, strip_offset=0, strip_byte_count=0
          ),
          err_regex=r'Cannot open TIFF file due to missing codec of code 65535',
      ),
      dict(
          testcase_name='uint33',
          entries=_minimal_1x1_entries(
              bits_per_sample=33,
              strip_offset=_TailStripOffsets(),
              strip_byte_count=8,
          ),
          err_regex=r'Unsupported TIFF configuration',
      ),
      dict(
          testcase_name='toomanyblocks',
          entries=(
              (256, _TIFF_SHORT, 1, 60000),
              (257, _TIFF_SHORT, 1, 60000),
              (258, _TIFF_SHORT, 1, 8),
              (259, _TIFF_SHORT, 1, 1),
              (262, _TIFF_SHORT, 1, 1),
              (277, _TIFF_SHORT, 1, 1),
              (284, _TIFF_SHORT, 1, 1),
              (322, _TIFF_SHORT, 1, 1),
              (323, _TIFF_SHORT, 1, 1),
              (324, _TIFF_LONG, 1, 0),
              (325, _TIFF_LONG, 1, 0),
              (339, _TIFF_SHORT, 1, 1),
          ),
          err_regex=r'Too many blocks: 60000 x 60000',
      ),
      dict(
          testcase_name='toomanyblocks_separate',
          entries=(
              (256, _TIFF_SHORT, 1, 32767),
              (257, _TIFF_SHORT, 1, 65535),
              (258, _TIFF_SHORT, 1, 8),
              (259, _TIFF_SHORT, 1, 1),
              (262, _TIFF_SHORT, 1, 1),
              (277, _TIFF_SHORT, 1, 2),
              (284, _TIFF_SHORT, 1, 2),
              (322, _TIFF_SHORT, 1, 1),
              (323, _TIFF_SHORT, 1, 1),
              (324, _TIFF_LONG, 1, 0),
              (325, _TIFF_LONG, 1, 0),
              (339, _TIFF_SHORT, 1, 1),
          ),
          err_regex=r'Too many blocks: 32767 x 65535 x 2 bands',
      ),
      dict(
          testcase_name='unhandled_codec_jp2000',
          entries=_minimal_1x1_entries(
              compression=34712,
              strip_offset=_TailStripOffsets(),
              strip_byte_count=1,
          ),
          err_regex=r'missing codec JP2000',
      ),
      dict(
          testcase_name='unhandled_codec_unknown_code',
          entries=_minimal_1x1_entries(
              compression=44510,
              strip_offset=_TailStripOffsets(),
              strip_byte_count=1,
          ),
          err_regex=r'missing codec of code 44510',
      ),
  )
  def test_invalid_tiff_headers_raise(
      self,
      entries: Sequence[tuple[int, int, int, int | bytes | _TailStripOffsets]],
      err_regex: str,
  ) -> None:
    vsi_dir = self._vsimem_dir('invalid_headers')
    filepath = f'{vsi_dir}/bad.tif'
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00' * 16)
    )
    with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, err_regex):
      self._open_readonly(filepath)

  # Upstream: test_tiff_read_second_image_width_above_32bit
  def test_tiff_read_second_image_width_above_32bit(self) -> None:
    vsi_dir = self._vsimem_dir('second_ifd_32bit')
    filepath = f'{vsi_dir}/second_image_width_above_32bit.tif'
    ifd1 = _minimal_1x1_entries(
        strip_offset=_TailStripOffsets(), strip_byte_count=1
    )
    ifd2 = _minimal_1x1_entries(
        width=0x80000000,
        width_type=_TIFF_LONG,
        strip_offset=_TailStripOffsets(),
        strip_byte_count=1,
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(ifd1, second_ifd=ifd2, tail_bytes=b'\x00')
    )
    with self._open_readonly(filepath) as ds:
      with gdal.quiet_errors():
        self.assertEmpty(ds.GetMetadata('SUBDATASETS'))

    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(
            RuntimeError, r'Too large image size: 2147483648 x 1'
        ),
    ):
      self._open_readonly(f'GTIFF_DIR:2:{filepath}')

  # Upstream: test_tiff_read_minimum_tiff_tags_no_warning / with_warning
  def test_tiff_read_minimum_tiff_tags(self) -> None:
    vsi_dir = self._vsimem_dir('min_tags')
    no_warn_path = f'{vsi_dir}/no_warn.tif'
    no_warn_entries = (
        (256, _TIFF_SHORT, 1, 1),
        (257, _TIFF_SHORT, 1, 1),
        (273, _TIFF_LONG, 1, 0),
        (279, _TIFF_LONG, 1, 1),
    )
    gdal.FileFromMemBuffer(no_warn_path, _build_classic_tiff(no_warn_entries))
    gdal.ErrorReset()
    with gdal.OpenEx(no_warn_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      ds.GetRasterBand(1).Checksum()
      self.assertEqual(gdal.GetLastErrorMsg(), '')

    warn_path = f'{vsi_dir}/with_warn.tif'
    warn_entries = (
        (256, _TIFF_SHORT, 1, 1),
        (257, _TIFF_SHORT, 1, 1),
        (273, _TIFF_LONG, 1, 0),
    )
    gdal.FileFromMemBuffer(warn_path, _build_classic_tiff(warn_entries))
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.OpenEx(warn_path, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')
        gdal.ErrorReset()
        ds.GetRasterBand(1).Checksum()
        self.assertEqual(gdal.GetLastErrorMsg(), '')

  # Upstream: test_tiff_read_leak_ZIPSetupDecode, excessive_memory_*,
  # corrupted_deflate_singlestrip, packbits_not_enough_data, corrupted_lzw
  @parameterized.named_parameters(
      dict(
          testcase_name='leak_zip_setup_decode',
          filename='leak-ZIPSetupDecode.tif',
          err_regex=r'PredictorSetup:"Predictor" value 255 not supported',
          check_read_raster=False,
      ),
      dict(
          testcase_name='excessive_memory_fill_strip',
          filename='excessive-memory-TIFFFillStrip.tif',
          err_regex=r'TIFFFillStrip:Read error.*got 1718 bytes, expected 83136',
          check_read_raster=False,
      ),
      dict(
          testcase_name='excessive_memory_fill_strip2',
          filename='excessive-memory-TIFFFillStrip2.tif',
          err_regex=(
              r'TIFFFillStrip:Read error.*got 0 bytes, expected 4253534167'
          ),
          check_read_raster=False,
      ),
      dict(
          testcase_name='excessive_memory_fill_tile',
          filename='excessive-memory-TIFFFillTile.tif',
          err_regex=r'TIFFFillTile:Read error.*got 512 bytes, expected 6656',
          check_read_raster=False,
      ),
      dict(
          testcase_name='corrupted_deflate_singlestrip',
          filename='corrupted_deflate_singlestrip.tif',
          err_regex=r'ZIPDecode:Decoding error|TIFFReadScanline\(\) failed',
          check_read_raster=False,
      ),
      dict(
          testcase_name='packbits_not_enough_data',
          filename='packbits-not-enough-data.tif',
          err_regex=r'PackBitsDecode:Not enough data for scanline 233',
          check_read_raster=False,
      ),
      dict(
          testcase_name='corrupted_lzw',
          filename='gtiff/lzw_corrupted.tif',
          err_regex=r'Using code not yet in table',
          check_read_raster=True,
      ),
  )
  def test_corrupted_streams_raise_cleanly(
      self, filename: str, err_regex: str, check_read_raster: bool
  ) -> None:
    filepath = self._test_path(filename)
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        with self.assertRaisesRegex(RuntimeError, err_regex):
          ds.GetRasterBand(1).Checksum()
        if check_read_raster:
          with self.assertRaisesRegex(RuntimeError, err_regex):
            ds.ReadRaster()

  # Upstream: test_tiff_read_big_strip
  def test_tiff_read_big_strip(self) -> None:
    vsi_dir = self._vsimem_dir('big_strip')
    out_path = f'{vsi_dir}/big_strip.tif'
    with gdal.Translate(
        out_path,
        self._test_path('byte.tif'),
        width=10000,
        height=2000,
        outputType=gdal.GDT_Float32,
        resampleAlg=gdal.GRIORA_Bilinear,
        creationOptions=['COMPRESS=LZW', 'BLOCKYSIZE=2000'],
    ):
      pass
    with self._open_readonly(out_path) as ds:
      arr = self._assert_band(
          ds.GetRasterBand(1), 2676, (2000, 10000), np.float32, gdal.GDT_Float32
      )
      self.assertGreater(float(arr.max()), 200.0)

  # Upstream: test_tiff_read_big_strip_chunky_way
  def test_tiff_read_big_strip_chunky_way(self) -> None:
    vsi_dir = self._vsimem_dir('big_strip_chunky')
    out_path = f'{vsi_dir}/big_strip_chunky.tif'
    with gdal.Translate(
        out_path,
        self._test_path('byte.tif'),
        width=1000,
        height=2001,
        resampleAlg=gdal.GRIORA_Bilinear,
        creationOptions=['COMPRESS=LZW', 'BLOCKYSIZE=2001'],
    ):
      pass
    with self._open_readonly(out_path) as ds:
      arr = self._assert_band(
          ds.GetRasterBand(1), 38441, (2001, 1000), np.uint8
      )
      self.assertGreater(int(arr.max()), 200)

  # Upstream: test_tiff_read_big_tile
  def test_tiff_read_big_tile(self) -> None:
    vsi_dir = self._vsimem_dir('big_tile')
    out_path = f'{vsi_dir}/big_tile.tif'
    with gdal.Translate(
        out_path,
        self._test_path('byte.tif'),
        width=10000,
        height=2000,
        outputType=gdal.GDT_Float32,
        resampleAlg=gdal.GRIORA_Bilinear,
        creationOptions=[
            'COMPRESS=LZW',
            'TILED=YES',
            'BLOCKXSIZE=10000',
            'BLOCKYSIZE=2000',
        ],
    ):
      pass
    with self._open_readonly(out_path) as ds:
      arr = self._assert_band(
          ds.GetRasterBand(1), 2676, (2000, 10000), np.float32, gdal.GDT_Float32
      )
      self.assertGreater(float(arr.max()), 200.0)

  # Upstream: test_tiff_read_huge_number_strips
  def test_tiff_read_huge_number_strips(self) -> None:
    vsi_dir = self._vsimem_dir('huge_num_strips')
    filepath = f'{vsi_dir}/huge_num_strips.tif'
    entries = (
        (256, _TIFF_LONG, 1, 100000),
        (257, _TIFF_LONG, 1, struct.pack('<Q', 0x2067615401A0)),
        (258, _TIFF_SHORT, 1, 8),
        (259, _TIFF_SHORT, 1, 1),
        (262, _TIFF_SHORT, 1, 1),
        (
            273,
            _TIFF_LONG8,
            0x6F6F6F6F6F6F6F20,
            struct.pack('<Q', 0x6F6F6F6F6F6F6F6F),
        ),
        (278, _TIFF_SHORT, 1, 1),
        (279, _TIFF_LONG8, 100000, struct.pack('<Q', 252)),
    )
    gdal.FileFromMemBuffer(filepath, _build_bigtiff(entries))
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(
            RuntimeError, r'EvaluateIFDdatasizeReading:Too large IFD data size'
        ),
    ):
      self._open_readonly(filepath)

  # Upstream: test_tiff_read_huge_implied_number_strips
  def test_tiff_read_huge_implied_number_strips(self) -> None:
    vsi_dir = self._vsimem_dir('huge_implied')
    filepath = f'{vsi_dir}/huge_implied_number_strips.tif'
    ifd1 = (
        (256, _TIFF_SHORT, 1, 20),
        (257, _TIFF_LONG, 1, 20),
        (273, _TIFF_LONG, 1, 46),
    )
    ifd2 = (
        (254, _TIFF_SHORT, 1, 1),
        (256, _TIFF_SHORT, 1, 20),
        (257, _TIFF_LONG, 1, 0x7FFFFFFF),
        (262, _TIFF_LONG, 1, 6),
        (273, _TIFF_LONG, 1, 46),
        (277, _TIFF_LONG, 1, 0x2020),
    )
    gdal.FileFromMemBuffer(filepath, _build_classic_tiff(ifd1, second_ifd=ifd2))
    err_msgs: list[str] = []

    def _capture(err_class: int, err_no: int, msg: str) -> None:
      del err_class, err_no
      err_msgs.append(msg)

    gdal.ErrorReset()
    with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
      gdal.PushErrorHandler(_capture)
      try:
        ds = gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY)
        self.assertIsNotNone(ds)
        if ds is not None:
          with ds:
            self.assertEqual((ds.RasterXSize, ds.RasterYSize), (20, 20))
            self.assertEmpty(ds.GetMetadata('SUBDATASETS'))
      finally:
        gdal.PopErrorHandler()
    self.assertTrue(
        any(
            'Invalid td_samplesperpixel value' in msg
            or 'Cannot handle zero strip size' in msg
            for msg in err_msgs
        ),
        msg=err_msgs,
    )

    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(
            RuntimeError,
            r'Invalid td_samplesperpixel value|Cannot handle zero strip size',
        ),
    ):
      self._open_readonly(f'GTIFF_DIR:2:{filepath}')

  # Upstream: test_tiff_read_many_blocks
  def test_tiff_read_many_blocks(self) -> None:
    vsi_dir = self._vsimem_dir('many_blocks')
    filepath = f'{vsi_dir}/many_blocks.tif'
    with self.driver.Create(filepath, 1, 2000000, options=['BLOCKYSIZE=1']):
      pass
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      arr = self._assert_band(band, 0, (2000000, 1), np.uint8)
      self.assertEqual(int(arr.max()), 0)

  # Upstream: test_tiff_read_many_blocks_truncated
  def test_tiff_read_many_blocks_truncated(self) -> None:
    vsi_dir = self._vsimem_dir('many_blocks_trunc')
    filepath = f'{vsi_dir}/many_blocks_truncated.tif'
    entries = _minimal_1x1_entries(
        height=16777215,
        height_type=_TIFF_LONG,
        compression=8,
        strip_count=16777215,
        strip_offset=0,
        strip_byte_count_num=16777215,
        strip_byte_count=0,
    )
    gdal.FileFromMemBuffer(filepath, _build_classic_tiff(entries))
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        with self.assertRaisesRegex(
            RuntimeError, r'_TIFFFetchStrileValue:File too short'
        ):
          ds.GetRasterBand(1).GetMetadataItem('BLOCK_OFFSET_0_2000000', 'TIFF')

  # Upstream: test_tiff_read_size_of_stripbytecount_lower_than_stripcount
  def test_tiff_read_size_of_stripbytecount_lower_than_stripcount(self) -> None:
    vsi_dir = self._vsimem_dir('stripbytecount_lt')
    with self.subTest(case='two_of_three'):
      path1 = f'{vsi_dir}/two_of_three.tif'
      counts = struct.pack('<II', 1, 1)
      entries = _minimal_1x1_entries(
          height=3,
          strip_count=2,
          strip_offset=_TailStripOffsets(count=2),
          strip_byte_count_num=2,
          strip_byte_count=counts,
      )
      tail = b'abc'
      raw = _build_classic_tiff(entries, tail_bytes=tail)
      data_start = len(raw) - len(tail)
      gdal.FileFromMemBuffer(path1, raw)
      with self._open_readonly(path1) as ds:
        band = ds.GetRasterBand(1)
        self.assertEqual(
            band.GetMetadataItem('BLOCK_OFFSET_0_1', 'TIFF'),
            str(data_start + 1),
        )
        self.assertEqual(band.GetMetadataItem('BLOCK_SIZE_0_1', 'TIFF'), '1')
        self.assertIsNone(band.GetMetadataItem('BLOCK_OFFSET_0_2', 'TIFF'))
        self.assertIsNone(band.GetMetadataItem('BLOCK_SIZE_0_2', 'TIFF'))

    with self.subTest(case='one_of_three'):
      path2 = f'{vsi_dir}/one_of_three.tif'
      entries = _minimal_1x1_entries(
          height=3,
          strip_count=1,
          strip_offset=_TailStripOffsets(),
          strip_byte_count_num=1,
          strip_byte_count=1,
      )
      gdal.FileFromMemBuffer(
          path2, _build_classic_tiff(entries, tail_bytes=b'abc')
      )
      with self._open_readonly(path2) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadataItem('BLOCK_SIZE_0_0', 'TIFF'), '1'
        )

    with self.subTest(case='stripbytecounts_mismatch_stripoffsets'):
      path3 = f'{vsi_dir}/mismatch.tif'
      counts = struct.pack('<II', 1, 1)
      entries = _minimal_1x1_entries(
          height=3,
          strip_count=3,
          strip_offset=_TailStripOffsets(count=3),
          strip_byte_count_num=2,
          strip_byte_count=counts,
      )
      tail = b'abc'
      raw = _build_classic_tiff(entries, tail_bytes=tail)
      data_start = len(raw) - len(tail)
      gdal.FileFromMemBuffer(path3, raw)
      with self._open_readonly(path3) as ds:
        band = ds.GetRasterBand(1)
        self.assertEqual(
            band.GetMetadataItem('BLOCK_OFFSET_0_0', 'TIFF'), str(data_start)
        )
        self.assertEqual(band.GetMetadataItem('BLOCK_SIZE_0_0', 'TIFF'), '1')

  # Upstream: test_tiff_read_stripoffset_types
  @parameterized.named_parameters(
      dict(
          testcase_name='classic_one_block_byte',
          bigtiff=False,
          endian='<',
          tiled=False,
          offset_type=_TIFF_BYTE,
          count=1,
      ),
      dict(
          testcase_name='classic_one_block_long_le',
          bigtiff=False,
          endian='<',
          tiled=True,
          offset_type=_TIFF_LONG,
          count=1,
      ),
      dict(
          testcase_name='classic_one_block_long_be',
          bigtiff=False,
          endian='>',
          tiled=True,
          offset_type=_TIFF_LONG,
          count=1,
      ),
      dict(
          testcase_name='classic_one_strip_long_le',
          bigtiff=False,
          endian='<',
          tiled=False,
          offset_type=_TIFF_LONG,
          count=1,
      ),
      dict(
          testcase_name='classic_one_strip_long_be',
          bigtiff=False,
          endian='>',
          tiled=False,
          offset_type=_TIFF_LONG,
          count=1,
      ),
      dict(
          testcase_name='classic_two_strip_short_le',
          bigtiff=False,
          endian='<',
          tiled=False,
          offset_type=_TIFF_SHORT,
          count=2,
      ),
      dict(
          testcase_name='classic_two_strip_short_be',
          bigtiff=False,
          endian='>',
          tiled=False,
          offset_type=_TIFF_SHORT,
          count=2,
      ),
      dict(
          testcase_name='classic_four_strip_short_le',
          bigtiff=False,
          endian='<',
          tiled=False,
          offset_type=_TIFF_SHORT,
          count=4,
      ),
      dict(
          testcase_name='classic_four_strip_short_be',
          bigtiff=False,
          endian='>',
          tiled=False,
          offset_type=_TIFF_SHORT,
          count=4,
      ),
      dict(
          testcase_name='bigtiff_four_strip_short_le',
          bigtiff=True,
          endian='<',
          tiled=False,
          offset_type=_TIFF_SHORT,
          count=4,
      ),
      dict(
          testcase_name='bigtiff_four_strip_short_be',
          bigtiff=True,
          endian='>',
          tiled=False,
          offset_type=_TIFF_SHORT,
          count=4,
      ),
      dict(
          testcase_name='bigtiff_one_block_long8_le',
          bigtiff=True,
          endian='<',
          tiled=True,
          offset_type=_TIFF_LONG8,
          count=1,
      ),
      dict(
          testcase_name='bigtiff_one_block_long8_be',
          bigtiff=True,
          endian='>',
          tiled=True,
          offset_type=_TIFF_LONG8,
          count=1,
      ),
      dict(
          testcase_name='bigtiff_one_strip_long_le',
          bigtiff=True,
          endian='<',
          tiled=False,
          offset_type=_TIFF_LONG,
          count=1,
      ),
      dict(
          testcase_name='bigtiff_one_strip_long_be',
          bigtiff=True,
          endian='>',
          tiled=False,
          offset_type=_TIFF_LONG,
          count=1,
      ),
      dict(
          testcase_name='bigtiff_one_strip_long8_le',
          bigtiff=True,
          endian='<',
          tiled=False,
          offset_type=_TIFF_LONG8,
          count=1,
      ),
      dict(
          testcase_name='bigtiff_one_strip_long8_be',
          bigtiff=True,
          endian='>',
          tiled=False,
          offset_type=_TIFF_LONG8,
          count=1,
      ),
      dict(
          testcase_name='bigtiff_two_strip_long_le',
          bigtiff=True,
          endian='<',
          tiled=False,
          offset_type=_TIFF_LONG,
          count=2,
      ),
      dict(
          testcase_name='bigtiff_two_strip_long_be',
          bigtiff=True,
          endian='>',
          tiled=False,
          offset_type=_TIFF_LONG,
          count=2,
      ),
      dict(
          testcase_name='bigtiff_two_strip_long8_le',
          bigtiff=True,
          endian='<',
          tiled=False,
          offset_type=_TIFF_LONG8,
          count=2,
      ),
      dict(
          testcase_name='bigtiff_two_strip_long8_be',
          bigtiff=True,
          endian='>',
          tiled=False,
          offset_type=_TIFF_LONG8,
          count=2,
      ),
  )
  def test_tiff_read_stripoffset_types(
      self,
      bigtiff: bool,
      endian: str,
      tiled: bool,
      offset_type: int,
      count: int,
  ) -> None:
    vsi_dir = self._vsimem_dir('stripoffset_types')
    filepath = f'{vsi_dir}/test.tif'
    type_pack = {
        _TIFF_BYTE: 'B',
        _TIFF_SHORT: 'H',
        _TIFF_LONG: 'I',
        _TIFF_LONG8: 'Q',
    }
    bc_type = _TIFF_LONG8 if bigtiff else _TIFF_LONG
    off_spec = _TailStripOffsets(count=count, field_type=offset_type, step=1)
    if count == 1:
      bc_val: int | bytes = 256 if tiled else 1
    else:
      bc_val = struct.pack(
          f'{endian}{count}{type_pack[bc_type]}', *([1] * count)
      )

    if tiled:
      entries = [
          (256, _TIFF_SHORT, 1, 1),
          (257, _TIFF_SHORT, 1, 1),
          (258, _TIFF_SHORT, 1, 8),
          (259, _TIFF_SHORT, 1, 1),
          (262, _TIFF_SHORT, 1, 1),
          (277, _TIFF_SHORT, 1, 1),
          (284, _TIFF_SHORT, 1, 1),
          (322, _TIFF_SHORT, 1, 16),
          (323, _TIFF_SHORT, 1, 16),
          (324, offset_type, 1, off_spec),
          (325, bc_type, 1, bc_val),
          (339, _TIFF_SHORT, 1, 1),
      ]
      tail = b'\x73' + b'\x00' * 255
      expected_arr = np.array([[0x73]], dtype=np.uint8)
    else:
      entries = [
          (256, _TIFF_SHORT, 1, 1),
          (257, _TIFF_SHORT, 1, count),
          (258, _TIFF_SHORT, 1, 8),
          (259, _TIFF_SHORT, 1, 1),
          (262, _TIFF_SHORT, 1, 1),
          (273, offset_type, count, off_spec),
          (277, _TIFF_SHORT, 1, 1),
          (278, _TIFF_SHORT, 1, 1),
          (279, bc_type, count, bc_val),
          (284, _TIFF_SHORT, 1, 1),
          (339, _TIFF_SHORT, 1, 1),
      ]
      tail = bytes(range(100, 100 + count))
      expected_arr = np.arange(100, 100 + count, dtype=np.uint8).reshape(
          count, 1
      )

    raw = (
        _build_bigtiff(entries, endian=endian, tail_bytes=tail)
        if bigtiff
        else _build_classic_tiff(entries, endian=endian, tail_bytes=tail)
    )
    data_start = len(raw) - len(tail)
    expected_offsets = [data_start + i for i in range(count)]
    gdal.FileFromMemBuffer(filepath, raw)

    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      offsets = [
          int(band.GetMetadataItem(f'BLOCK_OFFSET_0_{row}', 'TIFF'))
          for row in range(count)
      ]
      self.assertEqual(offsets, expected_offsets)
      self.assertIsNone(band.GetMetadataItem(f'BLOCK_OFFSET_0_{count}', 'TIFF'))
      np.testing.assert_array_equal(band.ReadAsArray(), expected_arr)

  # Upstream: test_tiff_read_progressive_jpeg_denial_of_service
  def test_tiff_read_progressive_jpeg_denial_of_service(self) -> None:
    zip_path = self._test_path('eofloop_valid_huff.tif.zip')
    with mock.patch.dict(
        os.environ,
        {
            'JPEGMEM': '10M',
            'LIBTIFF_JPEG_MAX_ALLOWED_SCAN_NUMBER': '1000',
        },
    ):
      with gdal.quiet_errors():
        with self._open_readonly(f'/vsizip/{zip_path}') as ds:
          with self.assertRaisesRegex(
              RuntimeError,
              r'Reading this image would require libjpeg to allocate at least '
              r'\d+ bytes',
          ):
            ds.GetRasterBand(1).Checksum()

    with mock.patch.dict(
        os.environ,
        {
            'LIBTIFF_ALLOW_LARGE_LIBJPEG_MEM_ALLOC': 'YES',
            'LIBTIFF_JPEG_MAX_ALLOWED_SCAN_NUMBER': '10',
        },
    ):
      with self._open_readonly(f'/vsizip/{zip_path}') as ds:
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(
                RuntimeError,
                r'TIFFjpeg_progress_monitor:Scan number \d+ exceeds maximum'
                r' scans'
                r' \(10\)',
            ),
        ):
          ds.GetRasterBand(1).Checksum()

  # Upstream: test_tiff_read_old_style_lzw
  def test_tiff_read_old_style_lzw(self) -> None:
    filepath = self._test_path('quad-lzw-old-style.tif')
    with self._open_readonly(filepath) as ds:
      with gdal.quiet_errors():
        arr = self._assert_band(
            ds.GetRasterBand(1), 34282, (384, 512), np.uint8
        )
        self.assertGreater(int(arr.max()), 0)

  # Upstream: test_tiff_read_mmap_interface
  @parameterized.named_parameters(
      dict(
          testcase_name='uncompressed_strip',
          options=[],
          err_regex=r'TIFFFillStrip:Read error',
      ),
      dict(
          testcase_name='uncompressed_tiled',
          options=['TILED=YES'],
          err_regex=r'TIFFReadEncodedTile\(\) failed',
      ),
      dict(
          testcase_name='lzw_strip',
          options=['COMPRESS=LZW'],
          err_regex=r'TIFFFillStrip:Read error',
      ),
      dict(
          testcase_name='lzw_tiled',
          options=['COMPRESS=LZW', 'TILED=YES'],
          err_regex=r'TIFFReadEncodedTile\(\) failed',
      ),
  )
  def test_tiff_read_mmap_interface(
      self, options: Sequence[str], err_regex: str
  ) -> None:
    vsi_dir = self._vsimem_dir('mmap')
    tmpfile = f'{vsi_dir}/mmap.tif'
    with self._open_readonly(self._test_path('byte.tif')) as src_ds:
      expected_arr = src_ds.GetRasterBand(1).ReadAsArray()
      if expected_arr is None:
        self.fail('Failed to read reference byte.tif')
      with self.driver.CreateCopy(tmpfile, src_ds, options=list(options)):
        pass

    with gdal.config_option('GTIFF_USE_MMAP', 'YES'):
      with self._open_readonly(tmpfile) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        np.testing.assert_array_equal(
            ds.GetRasterBand(1).ReadAsArray(), expected_arr
        )

    size = gdal.VSIStatL(tmpfile).size
    with gdal.VSIFile(tmpfile, 'rb') as f:
      truncated = f.read(size - 1)
    with gdal.VSIFile(tmpfile, 'wb') as f:
      f.write(truncated)

    with gdal.config_option('GTIFF_USE_MMAP', 'YES'):
      with self._open_readonly(tmpfile) as ds:
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(RuntimeError, err_regex),
        ):
          ds.GetRasterBand(1).Checksum()

  # Upstream: test_tiff_read_jpeg_too_big_last_stripe
  @parameterized.named_parameters(
      dict(
          testcase_name='grayscale', filename='tif_jpeg_too_big_last_stripe.tif'
      ),
      dict(
          testcase_name='ycbcr',
          filename='tif_jpeg_ycbcr_too_big_last_stripe.tif',
      ),
  )
  def test_tiff_read_jpeg_too_big_last_stripe(self, filename: str) -> None:
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      with gdal.quiet_errors():
        arr = self._assert_band(ds.GetRasterBand(1), 4557, (19, 20), np.uint8)
        self.assertGreater(int(arr.max()), 0)

  # Upstream: test_tiff_read_negative_scaley
  def test_tiff_read_negative_scaley(self) -> None:
    vsi_dir = self._vsimem_dir('negative_scaley')
    filepath = f'{vsi_dir}/negative_scaley.tif'
    pixel_scale = struct.pack('<3d', 60.0, -60.0, 0.0)
    tiepoints = struct.pack('<6d', 0.0, 0.0, 0.0, 440720.0, 3751320.0, 0.0)
    entries = _minimal_1x1_entries(
        strip_offset=_TailStripOffsets(),
        strip_byte_count=1,
        extra_entries=(
            (33550, _TIFF_DOUBLE, 3, pixel_scale),
            (33922, _TIFF_DOUBLE, 6, tiepoints),
        ),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00')
    )

    gdal.ErrorReset()
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        self._assert_geotransform(ds, BYTE_GEOTRANSFORM)
        self.assertIn(
            'File with negative value for ScaleY in GeoPixelScale tag',
            gdal.GetLastErrorMsg(),
        )
    gdal.ErrorReset()
    with gdal.config_option('GTIFF_HONOUR_NEGATIVE_SCALEY', 'NO'):
      with self._open_readonly(filepath) as ds:
        self._assert_geotransform(ds, BYTE_GEOTRANSFORM)
        self.assertEqual(gdal.GetLastErrorMsg(), '')
    gdal.ErrorReset()
    with gdal.config_option('GTIFF_HONOUR_NEGATIVE_SCALEY', 'YES'):
      with self._open_readonly(filepath) as ds:
        self._assert_geotransform(
            ds, (440720.0, 60.0, 0.0, 3751320.0, 0.0, 60.0)
        )
        self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_zstd(self) -> None:
    with self.subTest(case='utmsmall_zstd'):
      filepath = self._test_path('utmsmall-zstd.tif')
      with self._open_readonly(filepath) as ds:
        self.assertEqual(
            ds.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'), 'BAND'
        )
        self.assertEqual(
            ds.GetMetadataItem('COMPRESSION', 'IMAGE_STRUCTURE'), 'ZSTD'
        )
        self.assertEqual(
            ds.GetMetadataItem('PREDICTOR', 'IMAGE_STRUCTURE'), '2'
        )
        arr = self._assert_band(
            ds.GetRasterBand(1), 50054, (100, 100), np.uint8
        )
        self.assertGreater(int(arr.max()), 0)

    # Upstream: test_tiff_read_zstd
    with self.subTest(case='byte_zstd'):
      filepath = self._test_path('byte_zstd.tif')
      with self._open_readonly(filepath) as ds:
        self.assertEqual(
            ds.GetMetadataItem('COMPRESSION', 'IMAGE_STRUCTURE'), 'ZSTD'
        )
        self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_zstd_corrupted / corrupted2
  @parameterized.named_parameters(
      dict(
          testcase_name='corrupted_1',
          filename='byte_zstd_corrupted.tif',
          err_regex=(
              r'ZSTDDecode:Error in ZSTD_decompressStream\(\): '
              r'Data corruption detected'
          ),
      ),
      dict(
          testcase_name='corrupted_2',
          filename='byte_zstd_corrupted2.tif',
          err_regex=(
              r'ZSTDDecode:Error in ZSTD_decompressStream\(\): '
              r'Unknown frame descriptor'
          ),
      ),
  )
  def test_tiff_read_zstd_corrupted(
      self, filename: str, err_regex: str
  ) -> None:
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, err_regex):
        ds.GetRasterBand(1).Checksum()

  # Upstream: test_tiff_read_webp
  def test_tiff_read_webp(self) -> None:
    filepath = self._test_path('tif_webp.tif')
    with self._open_readonly(filepath) as ds:
      self.assertEqual(
          ds.GetMetadataItem('COMPRESSION', 'IMAGE_STRUCTURE'), 'WEBP'
      )
      band = ds.GetRasterBand(1)
      stats = band.GetStatistics(0, 1)
      np.testing.assert_allclose(
          stats, (0.0, 215.0, 66.38, 47.186), atol=1.0, rtol=0.0
      )
      arr = band.ReadAsArray()
      if arr is None:
        self.fail('Expected non-None ReadAsArray() result')
      self.assertEqual(arr.shape, (50, 50))
      self.assertEqual(arr.dtype, np.uint8)

  # Upstream: test_tiff_read_webp_huge_single_strip
  def test_tiff_read_webp_huge_single_strip(self) -> None:
    filepath = self._test_path('tif_webp_huge_single_strip.tif')
    with self._open_readonly(filepath) as ds:
      arr = self._assert_band(ds.GetRasterBand(1), 24409, (2001, 1), np.uint8)
      self.assertGreater(int(arr.max()), 0)

  # Upstream: test_tiff_read_1bit_2bands
  def test_tiff_read_1bit_2bands(self) -> None:
    filepath = self._test_path('1bit_2bands.tif')
    with self._open_readonly(filepath) as ds:
      arr1 = self._assert_band(ds.GetRasterBand(1), 200, (32, 32), np.uint8)
      arr2 = self._assert_band(ds.GetRasterBand(2), 824, (32, 32), np.uint8)
      self.assertEqual(int(arr1.max()), 1)
      self.assertEqual(int(arr2.max()), 1)

  # Upstream: test_tiff_read_lerc
  def test_tiff_read_lerc(self) -> None:
    vsi_dir = self._vsimem_dir('lerc')
    filepath = f'{vsi_dir}/byte_lerc.tif'
    with gdal.Translate(
        filepath,
        self._test_path('byte.tif'),
        creationOptions=['COMPRESS=LERC'],
    ):
      pass
    with self._open_readonly(filepath) as ds:
      self.assertEqual(
          ds.GetMetadataItem('COMPRESSION', 'IMAGE_STRUCTURE'), 'LERC'
      )
      self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_overview_of_external_mask
  def test_tiff_read_overview_of_external_mask(self) -> None:
    vsi_dir = self._vsimem_dir('ext_mask_ovr')
    filename = f'{vsi_dir}/masked.tif'
    with gdal.config_option('GDAL_TIFF_INTERNAL_MASK', 'NO'):
      with gdal.Translate(
          filename, self._test_path('byte.tif'), options='-b 1 -mask 1'
      ):
        pass
    with gdal.OpenEx(filename, gdal.OF_RASTER | gdal.OF_UPDATE) as ds:
      ds.BuildOverviews('CUBIC', overviewlist=[2])
    with gdal.OpenEx(
        f'{filename}.msk', gdal.OF_RASTER | gdal.OF_UPDATE
    ) as msk_ds:
      msk_ds.BuildOverviews('NEAREST', overviewlist=[2])

    with self._open_readonly(filename) as ds:
      band = ds.GetRasterBand(1)
      ovr = self._require_overview(band, 0)
      cs1 = ovr.GetMaskBand().Checksum()
      band_msk_ovr = self._require_overview(band.GetMaskBand(), 0)
      cs2 = band_msk_ovr.Checksum()
      self.assertEqual(cs1, cs2)
      self.assertEqual(ovr.GetMaskFlags(), gdal.GMF_PER_DATASET)

  # Upstream:
  # test_tiff_read_ModelTiepointTag_z_non_zero_but_ModelPixelScaleTag_z_zero
  def test_tiff_read_model_tiepoint_z_nonzero_pixel_scale_z_zero(self) -> None:
    vsi_dir = self._vsimem_dir('tiepoint_z')
    filepath = f'{vsi_dir}/tiepoint_z.tif'
    pixel_scale = struct.pack('<3d', 2.0, 2.0, 0.0)
    tiepoints = struct.pack('<6d', 0.0, 0.0, 0.0, 640000.0, 7450000.0, 12.4)
    geokeys = struct.pack('<8H', 1, 1, 0, 1, 1024, 0, 1, 1)
    entries = _minimal_1x1_entries(
        strip_offset=_TailStripOffsets(),
        strip_byte_count=1,
        extra_entries=(
            (33550, _TIFF_DOUBLE, 3, pixel_scale),
            (33922, _TIFF_DOUBLE, 6, tiepoints),
            (34735, _TIFF_SHORT, 8, geokeys),
        ),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00')
    )
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      self.assertIsNone(band.GetScale())
      self.assertIsNone(band.GetOffset())

  # Upstream: test_tiff_read_strip_larger_than_2GB
  def test_tiff_read_strip_larger_than_2gb(self) -> None:
    vsi_dir = self._vsimem_dir('strip_gt_2gb')
    filepath = f'{vsi_dir}/strip_larger_than_2GB_header.tif'
    strip_offsets = struct.pack('>2Q', 264, 2250000264)
    strip_byte_counts = struct.pack('>2Q', 2250000000, 500000000)
    entries = (
        (256, _TIFF_SHORT, 1, 50000),
        (257, _TIFF_SHORT, 1, 55000),
        (258, _TIFF_SHORT, 1, 8),
        (259, _TIFF_SHORT, 1, 1),
        (262, _TIFF_SHORT, 1, 1),
        (273, _TIFF_LONG8, 2, strip_offsets),
        (277, _TIFF_SHORT, 1, 1),
        (278, _TIFF_SHORT, 1, 45000),
        (279, _TIFF_LONG8, 2, strip_byte_counts),
        (339, _TIFF_SHORT, 1, 1),
    )
    gdal.FileFromMemBuffer(filepath, _build_bigtiff(entries, endian='>'))
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.GetBlockSize(), [50000, 10737])
      self.assertEqual(band.GetMetadataItem('BLOCK_OFFSET_0_0', 'TIFF'), '264')
      self.assertEqual(
          band.GetMetadataItem('BLOCK_SIZE_0_0', 'TIFF'), '536850000'
      )
      self.assertEqual(
          band.GetMetadataItem('BLOCK_OFFSET_0_1', 'TIFF'), '536850264'
      )
      self.assertEqual(
          band.GetMetadataItem('BLOCK_SIZE_0_1', 'TIFF'), '536850000'
      )
      self.assertEqual(
          band.GetMetadataItem('BLOCK_OFFSET_0_5', 'TIFF'), '2684250264'
      )
      self.assertEqual(
          band.GetMetadataItem('BLOCK_SIZE_0_5', 'TIFF'), '65750000'
      )

  # Upstream: test_tiff_read_deflate_4GB
  def test_tiff_read_deflate_4gb(self) -> None:
    zip_path = self._test_path('test_deflate_4GB.tif.zip')
    vsi_path = f'/vsizip/{zip_path}/test_deflate_4GB.tif'
    # Google3 executes on 64-bit targets where >2 GB strip chunks are openable.
    with self._open_readonly(vsi_path) as ds:
      data = ds.ReadRaster(
          0, 0, ds.RasterXSize, ds.RasterYSize, buf_xsize=20, buf_ysize=20
      )
      if data is None:
        self.fail('Expected non-None ReadRaster() result')
      arr = np.frombuffer(data, dtype=np.uint8).reshape(20, 20)
      np.testing.assert_array_equal(arr, np.full((20, 20), 127, dtype=np.uint8))

  # Upstream: test_tiff_read_cog_strile_arrays_zeroified_when_possible
  def test_tiff_read_cog_strile_arrays_zeroified_when_possible(self) -> None:
    filepath = self._test_path('cog_strile_arrays_zeroified_when_possible.tif')
    with gdal.config_option('GTIFF_HAS_OPTIMIZED_READ_MULTI_RANGE', 'YES'):
      with self._open_readonly(filepath) as ds:
        band = ds.GetRasterBand(1)
        arr = self._assert_band(band, 4873, (20, 20), np.uint8)
        np.testing.assert_array_equal(arr, np.full((20, 20), 255, np.uint8))
        mask_arr = self._assert_band(
            band.GetMaskBand(), 1222, (20, 20), np.uint8
        )
        self.assertEqual(int(mask_arr.max()), 255)

  # Upstream: test_tiff_GetMetadataDomainList
  def test_tiff_get_metadata_domain_list(self) -> None:
    filepath = self._test_path('byte.tif')
    with self._open_readonly(filepath) as ds:
      expected = {'', 'DERIVED_SUBDATASETS', 'IMAGE_STRUCTURE'}
      self.assertEqual(set(ds.GetMetadataDomainList()), expected)
      self.assertEqual(set(ds.GetMetadataDomainList()), expected)

  # Upstream: test_tiff_read_bigtiff_invalid_slong8_for_stripoffsets
  def test_tiff_read_bigtiff_invalid_slong8_for_stripoffsets(self) -> None:
    filepath = self._test_path(
        'byte_bigtiff_invalid_slong8_for_stripoffsets.tif'
    )
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_tiff_single_band_with_whitepoint_primarychroma_tags
  def test_tiff_read_single_band_whitepoint_primarychroma_tags(self) -> None:
    vsi_dir = self._vsimem_dir('whitepoint')
    filepath = f'{vsi_dir}/whitepoint.tif'
    whitepoint = struct.pack('<4I', 3127, 10000, 3290, 10000)
    primary_chroma = struct.pack(
        '<12I',
        6400,
        10000,
        3300,
        10000,
        3000,
        10000,
        6000,
        10000,
        1500,
        10000,
        600,
        10000,
    )
    entries = _minimal_1x1_entries(
        strip_offset=_TailStripOffsets(),
        strip_byte_count=1,
        extra_entries=(
            (318, _TIFF_RATIONAL, 2, whitepoint),
            (319, _TIFF_RATIONAL, 6, primary_chroma),
        ),
    )
    gdal.FileFromMemBuffer(
        filepath, _build_classic_tiff(entries, tail_bytes=b'\x00')
    )
    with self._open_readonly(filepath) as ds:
      self.assertEmpty(ds.GetMetadata('COLOR_PROFILE'))

  # Upstream: test_tiff_read_geodetic_tiff_grid
  def test_tiff_read_geodetic_tiff_grid(self) -> None:
    filepath = self._test_path('test_hgrid_with_subgrid.tif')
    with self._open_readonly(filepath) as ds:
      self.assertEqual(
          ds.GetSubDatasets()[0][1], 'Page 1 (10P x 10L x 2B): CAwest'
      )

  # Upstream: test_tiff_read_utf8_encoding_issue_2903
  def test_tiff_read_utf8_encoding_issue_2903(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    stem = '\u00e4'
    tif_path = tmp_dir / f'{stem}.tif'
    tfw_path = tmp_dir / f'{stem}.tfw'
    with gdal.Translate(
        str(tif_path),
        self._test_path('byte.tif'),
        creationOptions=['PROFILE=BASELINE'],
    ):
      pass
    tfw_path.write_text('1,\n0,\n0,\n-1,\n100,\n200\n', encoding='utf-8')
    with self._open_readonly(str(tif_path)) as ds:
      self._assert_geotransform(ds, (99.5, 1.0, 0.0, 200.5, 0.0, -1.0))
      self.assertCountEqual(ds.GetFileList(), [str(tif_path), str(tfw_path)])
      self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_overprecision_nodata_float32
  def test_tiff_read_overprecision_nodata_float32(self) -> None:
    vsi_dir = self._vsimem_dir('nodata_f32')
    filepath = f'{vsi_dir}/nodata_f32.tif'
    with self.driver.Create(filepath, 1, 1, 1, gdal.GDT_Float32) as ds:
      band = ds.GetRasterBand(1)
      band.SetNoDataValue(-3.4e38)
      band.Fill(-3.4e38)
    with self._open_readonly(filepath) as ds:
      band = ds.GetRasterBand(1)
      expected = struct.unpack('f', struct.pack('f', -3.4e38))[0]
      self.assertEqual(band.GetNoDataValue(), expected)
      raw = band.ReadRaster()
      if raw is None:
        self.fail('Expected non-None ReadRaster() result')
      pixel = struct.unpack('f', raw)[0]
      self.assertEqual(pixel, band.GetNoDataValue())

  # Upstream: test_tiff_jxl_read_for_files_created_before_6393
  def test_tiff_jxl_read_for_files_created_before_6393(self) -> None:
    gdal.ErrorReset()
    filepath = self._test_path('gtiff/jxl-rgbi.tif')
    filepath_orig = self._test_path('gtiff/rgba.tif')
    with (
        self._open_readonly(filepath) as ds,
        self._open_readonly(filepath_orig) as ds_orig,
    ):
      self.assertEqual(ds.RasterCount, ds_orig.RasterCount)
      for i in range(1, ds.RasterCount + 1):
        with self.subTest(band=i):
          self.assertEqual(
              ds.GetRasterBand(i).Checksum(),
              ds_orig.GetRasterBand(i).Checksum(),
          )
      np.testing.assert_array_equal(ds.ReadAsArray(), ds_orig.ReadAsArray())
      self.assertEqual(gdal.GetLastErrorMsg(), '')

  @parameterized.named_parameters(
      dict(
          testcase_name='deprecated_50002',
          filename='gtiff/byte_jxl_deprecated_50002.tif',
      ),
      dict(
          testcase_name='dng_1_7_52546',
          filename='gtiff/byte_jxl_dng_1_7_52546.tif',
      ),
  )
  def test_tiff_read_jxl_codec_variants(self, filename: str) -> None:
    """Tests reading legacy/DNG JXL TIFF compression codes."""
    filepath = self._test_path(filename)
    with self._open_readonly(filepath) as ds:
      self.assertEqual(
          ds.GetMetadataItem('COMPRESSION', 'IMAGE_STRUCTURE'), 'JXL'
      )
      self._assert_byte_matches(ds.GetRasterBand(1))

  # Upstream: test_tiff_read_multi_threaded
  @parameterized.named_parameters(
      dict(
          testcase_name='lzw_pred2_tiled_reopen',
          reopen=True,
          write_after_reopen=False,
          xsize=64,
          ysize=96,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=LZW',
              'PREDICTOR=2',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
          ),
      ),
      dict(
          testcase_name='lzw_u16_tiled_reopen_write',
          reopen=True,
          write_after_reopen=True,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_UInt16,
          creation_options=(
              'COMPRESS=LZW',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
          ),
      ),
      dict(
          testcase_name='lzw_band_tiled_reopen',
          reopen=True,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=LZW',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
              'INTERLEAVE=BAND',
          ),
      ),
      dict(
          testcase_name='lzw_band_minisblack_tiled_reopen_write',
          reopen=True,
          write_after_reopen=True,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=LZW',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
              'INTERLEAVE=BAND',
              'PHOTOMETRIC=MINISBLACK',
          ),
      ),
      dict(
          testcase_name='lzw_1band_tiled_reopen',
          reopen=True,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=1,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=LZW',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
              'INTERLEAVE=BAND',
          ),
      ),
      dict(
          testcase_name='lzw_tiled_direct',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=LZW',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
          ),
      ),
      dict(
          testcase_name='lzw_band_tiled_direct',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=LZW',
              'TILED=YES',
              'BLOCKXSIZE=16',
              'BLOCKYSIZE=32',
              'INTERLEAVE=BAND',
          ),
      ),
      dict(
          testcase_name='lzw_strip_unaligned',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=LZW', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='lzw_5bands_strip_aligned',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=5,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=LZW', 'BLOCKYSIZE=50'),
      ),
      dict(
          testcase_name='none',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=NONE', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='deflate',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=DEFLATE', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='zstd',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=ZSTD', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='lzma',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=LZMA', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='webp_lossless',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=WEBP',
              'WEBP_LOSSLESS=YES',
              'BLOCKYSIZE=18',
          ),
      ),
      dict(
          testcase_name='jpeg_ycbcr',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=(
              'COMPRESS=JPEG',
              'JPEG_QUALITY=95',
              'PHOTOMETRIC=YCBCR',
              'BLOCKYSIZE=16',
          ),
      ),
      dict(
          testcase_name='jpeg_1band',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=1,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=JPEG', 'BLOCKYSIZE=16'),
      ),
      dict(
          testcase_name='lerc',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=LERC', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='jxl',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=JXL', 'BLOCKYSIZE=18'),
      ),
      dict(
          testcase_name='packbits',
          reopen=False,
          write_after_reopen=False,
          xsize=100,
          ysize=100,
          nbands=3,
          dtype=gdal.GDT_Byte,
          creation_options=('COMPRESS=PACKBITS', 'BLOCKYSIZE=18'),
      ),
  )
  def test_tiff_read_multi_threaded(
      self,
      reopen: bool,
      write_after_reopen: bool,
      xsize: int,
      ysize: int,
      nbands: int,
      dtype: int,
      creation_options: Sequence[str],
  ) -> None:
    method = creation_options[0][len('COMPRESS=') :]
    mem_drv = self._require_driver_by_name('MEM')
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    tmpfile = str(tmp_dir / 'multithreaded.tif')

    rows = np.arange(ysize, dtype=np.int32)[:, None]
    cols = np.arange(xsize, dtype=np.int32)[None, :]
    with mem_drv.Create('', xsize, ysize, nbands, dtype) as ref_ds:
      for band_idx in range(nbands):
        arr = ((band_idx * 10 + rows + cols) % 256).astype(np.uint8)
        ref_ds.GetRasterBand(band_idx + 1).WriteArray(arr)

      tsan_build = os.environ.get('GDAL_TSAN_BUILD') == '1'
      opts = list(creation_options)
      if tsan_build:
        # Workaround for upstream GDAL bit-field race in
        # GTiffDataset::FlushCacheInternal() clearing m_bLoadedBlockDirty before
        # WaitCompletion(), racing with worker threads in
        # RestoreVolatileParameters() (gtiffdataset.h:256,
        # gtiffdataset_write.cpp:2188; untracked upstream). Keep writes
        # single-threaded under TSan and test multi-threaded reads via
        # OF_READONLY.
        with self.driver.Create(
            tmpfile, xsize, ysize, nbands, dtype, opts + ['NUM_THREADS=1']
        ) as ds_w:
          ds_w.WriteRaster(0, 0, xsize, ysize, ref_ds.ReadRaster())
        open_cm = gdal.OpenEx(
            tmpfile,
            gdal.OF_RASTER | gdal.OF_READONLY,
            open_options=['NUM_THREADS=ALL_CPUS'],
        )
      elif reopen:
        with self.driver.Create(
            tmpfile, xsize, ysize, nbands, dtype, opts
        ) as ds_w:
          ds_w.WriteRaster(0, 0, xsize, ysize, ref_ds.ReadRaster())
        open_cm = gdal.OpenEx(
            tmpfile,
            gdal.OF_RASTER | gdal.OF_UPDATE,
            open_options=['NUM_THREADS=ALL_CPUS'],
        )
      else:
        open_cm = self.driver.Create(
            tmpfile,
            xsize,
            ysize,
            nbands,
            dtype,
            opts + ['NUM_THREADS=ALL_CPUS'],
        )

      with open_cm as ds:
        if not tsan_build:
          if not reopen:
            ds.WriteRaster(0, 0, xsize, ysize, ref_ds.ReadRaster())
          elif write_after_reopen:
            x_off = xsize // 4
            y_off = ysize // 4
            xw = xsize - x_off
            yw = ysize - y_off
            ds.WriteRaster(
                x_off, y_off, xw, yw, ref_ds.ReadRaster(x_off, y_off, xw, yw)
            )

        pixel_size = gdal.GetDataTypeSize(dtype) // 8
        if method == 'JPEG':
          with mem_drv.Create('', xsize, ysize, nbands, dtype) as tmp_ds:
            tmp_ds.WriteRaster(0, 0, xsize, ysize, ds.ReadRaster())
            for i in range(nbands):
              # Allow <=1.0 stats drift for lossy JPEG compression.
              np.testing.assert_allclose(
                  tmp_ds.GetRasterBand(i + 1).ComputeStatistics(0),
                  ref_ds.GetRasterBand(i + 1).ComputeStatistics(0),
                  atol=1.0,
                  rtol=0.0,
              )
          return

        self.assertEqual(ds.ReadRaster(), ref_ds.ReadRaster())
        # Second read exercises GDAL's in-memory block cache hit path.
        self.assertEqual(ds.ReadRaster(), ref_ds.ReadRaster())
        self.assertEqual(
            ds.ReadRaster(buf_type=gdal.GDT_Byte),
            ref_ds.ReadRaster(buf_type=gdal.GDT_Byte),
        )
        self.assertEqual(
            ds.ReadRaster(buf_xsize=xsize // 2),
            ref_ds.ReadRaster(buf_xsize=xsize // 2),
        )
        self.assertEqual(
            ds.ReadRaster(
                buf_pixel_space=nbands * pixel_size, buf_band_space=pixel_size
            ),
            ref_ds.ReadRaster(
                buf_pixel_space=nbands * pixel_size, buf_band_space=pixel_size
            ),
        )
        for i in range(1, 1 + nbands):
          self.assertEqual(
              ds.GetRasterBand(i).ReadRaster(),
              ref_ds.GetRasterBand(i).ReadRaster(),
          )
          self.assertEqual(
              ds.GetRasterBand(i).ReadRaster(),
              ref_ds.GetRasterBand(i).ReadRaster(),
          )
        ds.FlushCache()
        inv_bands = [i + 1 for i in range(nbands)][::-1]
        self.assertEqual(
            ds.ReadRaster(band_list=inv_bands),
            ref_ds.ReadRaster(band_list=inv_bands),
        )
        self.assertEqual(
            ds.ReadRaster(band_list=inv_bands),
            ref_ds.ReadRaster(band_list=inv_bands),
        )
        ds.FlushCache()
        bx, by = ds.GetRasterBand(1).GetBlockSize()
        if bx < xsize:
          self.assertEqual(
              ds.ReadRaster(bx, by, 2 * bx, 2 * by),
              ref_ds.ReadRaster(bx, by, 2 * bx, 2 * by),
          )
        self.assertEqual(
            ds.ReadRaster(20, 40, 35, 50), ref_ds.ReadRaster(20, 40, 35, 50)
        )
        self.assertEqual(
            ds.ReadRaster(1, 1, xsize - 2, ysize - 2),
            ref_ds.ReadRaster(1, 1, xsize - 2, ysize - 2),
        )
        self.assertEqual(
            ds.ReadRaster(
                1,
                1,
                xsize - 2,
                ysize - 2,
                buf_pixel_space=nbands * pixel_size,
                buf_band_space=pixel_size,
            ),
            ref_ds.ReadRaster(
                1,
                1,
                xsize - 2,
                ysize - 2,
                buf_pixel_space=nbands * pixel_size,
                buf_band_space=pixel_size,
            ),
        )

  # Upstream: test_tiff_warning_get_metadata_item_PIXELTYPE
  def test_tiff_warning_get_metadata_item_pixeltype(self) -> None:
    filepath = self._test_path('byte.tif')
    gdal.ErrorReset()
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      with gdal.quiet_errors():
        ds.GetRasterBand(1).GetMetadataItem('PIXELTYPE', 'IMAGE_STRUCTURE')
      err_msg = gdal.GetLastErrorMsg()
    self.assertEqual(
        err_msg,
        'Starting with GDAL 3.7, PIXELTYPE=SIGNEDBYTE is no longer used to '
        'signal signed 8-bit raster. Change your code to test for the new '
        'GDT_Int8 data type instead.',
    )

  # Upstream: test_tiff_read_projection_from_esri_xml & get_file_list
  def test_tiff_read_projection_from_esri_xml(self) -> None:
    tfw = self._test_path('gtiff/projection_from_esri_xml.tfw')
    xml = self._test_path('gtiff/projection_from_esri_xml.xml')
    tif = self._test_path('gtiff/projection_from_esri_xml.tif')
    with self._open_readonly(tif) as ds:
      srs = self._require_srs(ds)
      self.assertEqual(srs.GetAuthorityName(None), 'EPSG')
      self.assertEqual(srs.GetAuthorityCode(None), '25833')
      self._assert_geotransform(
          ds, (250000.00001, 0.2, 0.0, 5887000.0, 0.0, -0.2)
      )
      self.assertCountEqual(ds.GetFileList(), [tif, tfw, xml])

  # Upstream: test_tiff_read_webp_lossless_rgba_alpha_fully_opaque
  def test_tiff_read_webp_lossless_rgba_alpha_fully_opaque(self) -> None:
    filepath = self._test_path(
        'gtiff/webp_lossless_rgba_alpha_fully_opaque.tif'
    )
    with self._open_readonly(filepath) as ds:
      for i in range(3):
        with self.subTest(band=i + 1):
          self._assert_byte_matches(ds.GetRasterBand(i + 1))
      alpha = self._assert_band(ds.GetRasterBand(4), 4873, (20, 20), np.uint8)
      np.testing.assert_array_equal(alpha, np.full((20, 20), 255, np.uint8))

  # Upstream: test_tiff_read_jpeg_cached_multi_range_issue_9563
  def test_tiff_read_jpeg_cached_multi_range_issue_9563(self) -> None:
    vsi_dir = self._vsimem_dir('issue_9563')
    blank = f'{vsi_dir}/blank.tif'
    with self.driver.Create(
        blank, 128, 128, 3, options=['SPARSE_OK=YES']
    ) as ds:
      ds.SetGeoTransform([
          -91.42822265625,
          (-91.40625 - -91.42822265625) / 128,
          0.0,
          15.205078125,
          0.0,
          (15.18310546875 - 15.205078125) / 128,
      ])
      ds.CreateMaskBand(gdal.GMF_PER_DATASET)

    cog = f'{vsi_dir}/cog.tif'
    with gdal.Translate(
        cog, blank, format='COG', creationOptions=['COMPRESS=JPEG']
    ):
      pass
    vrt = f'{vsi_dir}/vrt.vrt'
    with gdal.BuildVRT(
        vrt,
        [cog],
        outputBounds=(-91.494140625, 15.1171875, -91.40625, 15.205078125),
    ):
      pass
    out = f'{vsi_dir}/out.tif'
    with gdal.config_option('GTIFF_HAS_OPTIMIZED_READ_MULTI_RANGE', 'YES'):
      with gdal.Translate(
          out, vrt, xRes=0.000071806, yRes=0.000071806, format='COG'
      ):
        pass
    with self._open_readonly(out) as out_ds:
      self.assertGreater(out_ds.RasterXSize, 0)
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          out_ds.GetMetadataItem('LAYOUT', 'IMAGE_STRUCTURE'), 'COG'
      )
      arr = out_ds.ReadAsArray()
      if arr is None:
        self.fail('Expected non-None ReadAsArray() result')
      self.assertEqual(arr.shape, (3, out_ds.RasterYSize, out_ds.RasterXSize))
      self.assertEqual(arr.dtype, np.uint8)
      self.assertEqual(
          out_ds.GetRasterBand(1).GetMaskFlags(), gdal.GMF_PER_DATASET
      )

  # Upstream: test_tiff_read_missing_extrasamples_multi_threaded
  def test_tiff_read_missing_extrasamples_multi_threaded(self) -> None:
    filepath = self._test_path('gtiff/missing_extrasamples.tif')
    with gdal.quiet_errors():
      with self._open_readonly(filepath) as ds:
        gdal.ErrorReset()
        with gdal.config_option('GDAL_NUM_THREADS', '2'):
          data = ds.ReadRaster()
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        self.assertIsNotNone(data)
        if data is None:
          self.fail('Expected non-None ReadRaster() result')
        self.assertLen(data, ds.RasterXSize * ds.RasterYSize * ds.RasterCount)

  # Upstream: test_tiff_read_unrecognized_color_interpretation
  def test_tiff_read_unrecognized_color_interpretation(self) -> None:
    filepath = self._test_path('gtiff/unknown_colorinterp.tif')
    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_READONLY) as ds:
      band = ds.GetRasterBand(1)
      self.assertEqual(band.GetColorInterpretation(), gdal.GCI_Undefined)
      self.assertEqual(band.GetMetadataItem('COLOR_INTERPRETATION'), 'XXXX')

  # Upstream: test_tiff_read_ovr_dimap_pleiades
  def test_tiff_read_ovr_dimap_pleiades(self) -> None:
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    bundle_dir = tmp_dir / 'bundle'
    bundle_dir.mkdir()
    for src_rel, dst_name in (
        ('IMG_foo_R1C1.TIF', 'IMG_foo_R1C1.TIF'),
        ('DIM_foo.XML', 'DIM_foo.XML'),
        ('dimap2/single_component/RPC_foo.XML', 'RPC_foo.XML'),
    ):
      dst = bundle_dir / dst_name
      shutil.copyfile(self._test_path(src_rel), dst)
      dst.chmod(0o644)
    filename = str(bundle_dir / 'IMG_foo_R1C1.TIF')
    with self._open_readonly(filename) as ds:
      self.assertIn(str(bundle_dir / 'DIM_foo.XML'), ds.GetFileList())
      ds.BuildOverviews('NEAR', [2])
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
    with self._open_readonly(f'{filename}.ovr') as ds:
      self.assertEqual(ds.GetFileList(), [f'{filename}.ovr'])
    with self._open_readonly(filename) as ds:
      ds.BuildOverviews('', [])
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)
    self.assertTrue((bundle_dir / 'DIM_foo.XML').exists())

  # Upstream: test_tiff_read_vat_dbf
  @parameterized.named_parameters(
      dict(testcase_name='no', opt='NO'),
      dict(testcase_name='yes', opt='YES'),
  )
  def test_tiff_read_vat_dbf(self, opt: str) -> None:
    vsi_dir = self._vsimem_dir('vat_dbf')
    tif_path = f'{vsi_dir}/testrat.tif'
    dbf_path = f'{tif_path}.vat.dbf'
    self._copy_to_vsimem('byte.tif', tif_path)
    self._copy_to_vsimem('gtiff/testrat.tif.vat.dbf', dbf_path)

    with gdal.config_option('GDAL_DISABLE_READDIR_ON_OPEN', opt):
      with self._open_readonly(tif_path) as ds:
        band = ds.GetRasterBand(1)
        rat = self._require_band_rat(band)
        self.assertEqual(rat.GetColumnCount(), 9)
        self.assertEqual(rat.GetRowCount(), 2)
        self.assertEqual(
            [rat.GetNameOfCol(i) for i in range(9)],
            [
                'VALUE',
                'COUNT',
                'CLASS',
                'Red',
                'Green',
                'Blue',
                'OtherInt',
                'OtherReal',
                'OtherStr',
            ],
        )
        self.assertEqual(
            [rat.GetUsageOfCol(i) for i in range(9)],
            [
                gdal.GFU_MinMax,
                gdal.GFU_PixelCount,
                gdal.GFU_Name,
                gdal.GFU_Red,
                gdal.GFU_Green,
                gdal.GFU_Blue,
                gdal.GFU_Generic,
                gdal.GFU_Generic,
                gdal.GFU_Generic,
            ],
        )
        self.assertEqual(
            [rat.GetTypeOfCol(i) for i in range(9)],
            [
                gdal.GFT_Integer,
                gdal.GFT_Integer,
                gdal.GFT_String,
                gdal.GFT_Integer,
                gdal.GFT_Integer,
                gdal.GFT_Integer,
                gdal.GFT_Integer,
                gdal.GFT_Real,
                gdal.GFT_String,
            ],
        )
        self.assertEqual(rat.GetValueAsInt(0, 0), 1)
        self.assertEqual(rat.GetValueAsInt(0, 1), 10)
        self.assertEqual(rat.GetValueAsString(0, 2), 'my class')
        self.assertEqual(rat.GetValueAsInt(0, 3), 26)
        self.assertEqual(rat.GetValueAsInt(0, 4), 51)
        self.assertEqual(rat.GetValueAsInt(0, 5), 128)
        self.assertEqual(rat.GetValueAsInt(0, 6), 2)
        self.assertEqual(rat.GetValueAsDouble(0, 7), 2.5)
        self.assertEqual(rat.GetValueAsString(0, 8), 'foo')
        self.assertEqual(rat.GetValueAsInt(1, 0), 2)
        self.assertEqual(rat.GetValueAsString(1, 2), 'my class2')
        self.assertEqual(rat.GetValueAsString(1, 8), 'foo2')
        # Calling GetDefaultRAT() twice must continue returning the cached RAT.
        rat2 = self._require_band_rat(band)
        self.assertEqual(rat2.GetRowCount(), 2)
        self.assertIn(dbf_path, ds.GetFileList())

  # Upstream: test_tiff_read_vat_dbf (EMPTY_DIR)
  def test_tiff_read_vat_dbf_empty_dir(self) -> None:
    vsi_dir = self._vsimem_dir('vat_dbf_empty_dir')
    tif_path = f'{vsi_dir}/testrat.tif'
    dbf_path = f'{tif_path}.vat.dbf'
    self._copy_to_vsimem('byte.tif', tif_path)
    self._copy_to_vsimem('gtiff/testrat.tif.vat.dbf', dbf_path)

    with gdal.config_option('GDAL_DISABLE_READDIR_ON_OPEN', 'EMPTY_DIR'):
      with self._open_readonly(tif_path) as ds:
        self.assertIsNone(ds.GetRasterBand(1).GetDefaultRAT())

  # Upstream: test_tiff_read_no_vat_dbf
  @parameterized.named_parameters(
      dict(testcase_name='no', opt='NO'),
      dict(testcase_name='yes', opt='YES'),
      dict(testcase_name='empty_dir', opt='EMPTY_DIR'),
  )
  def test_tiff_read_no_vat_dbf(self, opt: str) -> None:
    with gdal.config_option('GDAL_DISABLE_READDIR_ON_OPEN', opt):
      with self._open_readonly(self._test_path('byte.tif')) as ds:
        self.assertIsNone(ds.GetRasterBand(1).GetDefaultRAT())

  # Upstream: test_tiff_read_corrupted_vat_dbf
  def test_tiff_read_corrupted_vat_dbf(self) -> None:
    vsi_dir = self._vsimem_dir('corrupted_vat_dbf')
    filename = f'{vsi_dir}/test.tif'
    with self.driver.Create(filename, 1, 1):
      pass
    gdal.FileFromMemBuffer(f'{filename}.vat.dbf', b'')
    with self._open_readonly(filename) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'not recognized as being in a supported file format',
          ),
      ):
        ds.GetRasterBand(1).GetDefaultRAT()

  # Upstream: test_tiff_read_vat_dbf_boolean_datetime
  def test_tiff_read_vat_dbf_boolean_datetime(self) -> None:
    vsi_dir = self._vsimem_dir('vat_dbf_bool_dt')
    filename = f'{vsi_dir}/test.tif'
    with self.driver.Create(filename, 1, 1):
      pass
    shp_drv = self._require_driver_by_name('ESRI Shapefile')
    with shp_drv.CreateVector(f'{filename}.vat.dbf') as ds:
      lyr = ds.CreateLayer('test.tif.vat', geom_type=ogr.wkbNone)
      fld_defn = ogr.FieldDefn('bool', ogr.OFTInteger)
      fld_defn.SetSubType(ogr.OFSTBoolean)
      lyr.CreateField(fld_defn)
      lyr.CreateField(ogr.FieldDefn('date', ogr.OFTDate))

    with self._open_readonly(filename) as ds:
      rat = self._require_band_rat(ds.GetRasterBand(1))
      self.assertEqual(rat.GetTypeOfCol(0), gdal.GFT_Boolean)
      self.assertEqual(rat.GetTypeOfCol(1), gdal.GFT_DateTime)

  def test_tiff_read_multithreaded_read_fresh_file(self) -> None:
    vsi_dir = self._vsimem_dir('mt_fresh')
    filename = f'{vsi_dir}/fresh.tif'
    with self.driver.Create(
        filename,
        xsize=100,
        ysize=100,
        bands=1,
        eType=gdal.GDT_Byte,
        options=['COMPRESS=DEFLATE', 'NUM_THREADS=2'],
    ) as ds_out:
      self.assertEqual(ds_out.ReadRaster(0, 0, 100, 100), b'\x00' * (100 * 100))

  def test_tiff_read_multithreaded_read_missing_tilebytecounts_and_offsets(
      self,
  ) -> None:
    filename = self._test_path('gtiff/missing_tilebytecounts_and_offsets.tif')
    with self._open_readonly(filename, open_options=['NUM_THREADS=2']) as ds:
      message = r'Error while getting location of block 0'
      with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, message):
        ds.ReadRaster()

  # Upstream: test_tiff_read_envi_hdr
  def test_tiff_read_envi_hdr(self) -> None:
    vsi_dir = self._vsimem_dir('envi_hdr')
    bin_path = f'{vsi_dir}/byte_envi.bin'
    hdr_path = f'{vsi_dir}/byte_envi.hdr'
    self._copy_to_vsimem('byte.tif', bin_path)
    self._copy_to_vsimem('gtiff/byte_envi.hdr', hdr_path)

    with gdal.config_option('GDAL_PAM_ENABLED', 'ON'):
      with self._open_readonly(bin_path) as ds:
        self.assertEqual(ds.GetFileList(), [bin_path, hdr_path])
      self.assertIsNone(gdal.VSIStatL(f'{bin_path}.aux.xml'))

      with self._open_readonly(bin_path) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadataDomainList(), ['', 'IMAGERY']
        )
      self.assertIsNone(gdal.VSIStatL(f'{bin_path}.aux.xml'))

      with self._open_readonly(bin_path) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadata_Dict(),
            {
                'good_band': 'true',
                'wavelength': '400.1',
                'wavelength_units': 'Nanometers',
            },
        )
      self.assertIsNone(gdal.VSIStatL(f'{bin_path}.aux.xml'))

      with self._open_readonly(bin_path) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadata_Dict('IMAGERY'),
            {
                'CENTRAL_WAVELENGTH_UM': '0.400',
                'FWHM_UM': '0.005',
            },
        )
      self.assertIsNone(gdal.VSIStatL(f'{bin_path}.aux.xml'))

      with self._open_readonly(bin_path) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadataItem('good_band'), 'true'
        )
      self.assertIsNone(gdal.VSIStatL(f'{bin_path}.aux.xml'))

      with self._open_readonly(bin_path) as ds:
        self.assertEqual(
            ds.GetRasterBand(1).GetMetadataItem(
                'CENTRAL_WAVELENGTH_UM', 'IMAGERY'
            ),
            '0.400',
        )
      self.assertIsNone(gdal.VSIStatL(f'{bin_path}.aux.xml'))

  # Upstream: test_tiff_read_json_ISIS3 (SetMetadata/GetMetadata subset
  # supported on pinned c5a57bae66; key-based GetMetadataItem(..., 'json:ISIS3')
  # subsetting added in upstream eb71a65b5d awaits GDAL upgrade).
  def test_tiff_read_json_isis3(self) -> None:
    vsi_dir = self._vsimem_dir('json_isis3')
    out_path = f'{vsi_dir}/isis3.tif'
    isis3_json = '{"IsisCube":{"Core":{"Dimensions":{"Samples":2,"Lines":2}}}}'
    with self.driver.Create(out_path, 2, 2) as ds:
      ds.SetMetadata([isis3_json], 'json:ISIS3')
    with self._open_readonly(out_path) as ds:
      self.assertIn('json:ISIS3', ds.GetMetadataDomainList())
      md = ds.GetMetadata('json:ISIS3')
      self.assertLen(md, 1)
      self.assertEqual(json.loads(md[0]), json.loads(isis3_json))

  # Upstream: test_tiff_read_overview_level_open_option_honor_*_EMPTY_DIR
  # (non-network /vsimem/ variant for internal overview selection).
  def test_tiff_read_overview_level_empty_dir(self) -> None:
    vsi_dir = self._vsimem_dir('ovr_empty_dir')
    tif_path = f'{vsi_dir}/ovr.tif'
    data = np.arange(16, dtype=np.uint8).reshape(4, 4)
    with self.driver.Create(tif_path, 4, 4, 1, gdal.GDT_Byte) as ds:
      ds.GetRasterBand(1).WriteArray(data)
      ds.BuildOverviews('NEAR', [2])
    gdal.ErrorReset()
    with gdal.config_option('GDAL_DISABLE_READDIR_ON_OPEN', 'EMPTY_DIR'):
      with self._open_readonly(
          tif_path, open_options=['OVERVIEW_LEVEL=0']
      ) as ds:
        self.assertEqual(ds.GetFileList(), [tif_path])
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        self.assertEqual((ds.RasterXSize, ds.RasterYSize), (2, 2))
        np.testing.assert_array_equal(
            ds.GetRasterBand(1).ReadAsArray(), data[0::2, 0::2]
        )


if __name__ == '__main__':
  googletest.main()
