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
# Copyright (c) 2022, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
"""Test the Zarr raster driver in GDAL.

Format is described here:

https://gdal.org/drivers/raster/zarr.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/zarr_driver.py
"""
import array
import base64
import contextlib
import glob
import json
import math
import os
import pathlib
import struct
from typing import Iterator

from osgeo import gdal
from osgeo import osr

import unittest
from absl.testing import parameterized
from google3.third_party.gdal.autotest2.python.gdrivers import gdrivers_util

DRIVER = gdrivers_util.ZARR_DRIVER
EXT = '.zarr'  # And '.zr3'

_gdal_data_type_to_array_type = {
    gdal.GDT_Int8: 'b',
    gdal.GDT_Byte: 'B',
    gdal.GDT_Int16: 'h',
    gdal.GDT_UInt16: 'H',
    gdal.GDT_Int32: 'i',
    gdal.GDT_UInt32: 'I',
    gdal.GDT_Int64: 'q',
    gdal.GDT_UInt64: 'Q',
    gdal.GDT_Float16: 'e',
    gdal.GDT_Float32: 'f',
    gdal.GDT_Float64: 'd',
    gdal.GDT_CFloat16: 'e',
    gdal.GDT_CFloat32: 'f',
    gdal.GDT_CFloat64: 'd',
}


@contextlib.contextmanager
def no_gdal_exceptions() -> Iterator[None]:
  try:
    gdal.DontUseExceptions()
    yield
  finally:
    gdal.UseExceptions()


class ZarrInfoTest(gdrivers_util.DriverTestCase):

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


class ZarrTest(gdrivers_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pytype: disable=signature-mismatch
    super().setUp(DRIVER, EXT)
    gdal.UseExceptions()
    gdal.ErrorReset()

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def test_zarr_compressor_availability(self):
    compressors = self.driver.GetMetadataItem('COMPRESSORS').split(',')

    self.assertIn('blosc', compressors)
    self.assertIn('gzip', compressors)
    self.assertIn('lz4', compressors)
    self.assertIn('lzma', compressors)
    self.assertIn('zlib', compressors)
    self.assertIn('zstd', compressors)

    blosc_compressors = self.driver.GetMetadataItem('BLOSC_COMPRESSORS').split(
        ','
    )

    self.assertIn('blosclz', blosc_compressors)
    self.assertIn('lz4', blosc_compressors)
    self.assertIn('lz4hc', blosc_compressors)
    self.assertIn('snappy', blosc_compressors)
    self.assertIn('zlib', blosc_compressors)
    self.assertIn('zstd', blosc_compressors)

  @parameterized.parameters([
      ('!b1', gdal.GDT_Byte, None, None),
      ('!i1', gdal.GDT_Int8, None, None),
      ('!i1', gdal.GDT_Int8, -1, -1),
      ('!u1', gdal.GDT_Byte, None, None),
      # Not really legit to have the fill_value as a str.
      ('!u1', gdal.GDT_Byte, '1', 1),
      ('<i2', gdal.GDT_Int16, None, None),
      ('>i2', gdal.GDT_Int16, None, None),
      ('<i4', gdal.GDT_Int32, None, None),
      ('>i4', gdal.GDT_Int32, None, None),
      ('<i8', gdal.GDT_Int64, None, None),
      ('<i8', gdal.GDT_Int64, -(1 << 63), -(1 << 63)),
      # Not really legit to have the fill_value as a str.
      ('<i8', gdal.GDT_Int64, str(-(1 << 63)), -(1 << 63)),
      ('>i8', gdal.GDT_Int64, None, None),
      ('<u2', gdal.GDT_UInt16, None, None),
      ('>u2', gdal.GDT_UInt16, None, None),
      ('<u4', gdal.GDT_UInt32, None, None),
      ('>u4', gdal.GDT_UInt32, None, None),
      ('<u4', gdal.GDT_UInt32, 4000000000, 4000000000),
      # Not really legit to have the fill_value as a str, but libjson-c can't
      # support numeric values in int64::max(), uint64::max() range.
      ('<u8', gdal.GDT_UInt64, str((1 << 64) - 1), (1 << 64) - 1),
      ('>u8', gdal.GDT_UInt64, None, None),
      # We would like to test these, but SWIG does not support float16 (yet?)
      # ["<f2", gdal.GDT_Float16, None, None],
      # [">f2", gdal.GDT_Float16, None, None],
      # ["<f2", gdal.GDT_Float16, 1.5, 1.5],
      # ["<f2", gdal.GDT_Float16, "NaN", float("nan")],
      # ["<f2", gdal.GDT_Float16, "Infinity", float("infinity")],
      # ["<f2", gdal.GDT_Float16, "-Infinity", float("-infinity")],
      ('<f4', gdal.GDT_Float32, None, None),
      ('>f4', gdal.GDT_Float32, None, None),
      ('<f4', gdal.GDT_Float32, 1.5, 1.5),
      ('<f4', gdal.GDT_Float32, 'NaN', math.nan),
      ('<f4', gdal.GDT_Float32, 'Infinity', math.inf),
      ('<f4', gdal.GDT_Float32, '-Infinity', -math.inf),
      ('<f8', gdal.GDT_Float64, None, None),
      ('>f8', gdal.GDT_Float64, None, None),
      ('<f8', gdal.GDT_Float64, 'NaN', math.nan),
      ('<f8', gdal.GDT_Float64, 'Infinity', math.inf),
      ('<f8', gdal.GDT_Float64, '-Infinity', -math.inf),
      # We would like to test these, but SWIG does not support complex32 (yet?)
      # ("<c4", gdal.GDT_CFloat16, None, None),
      # (">c4", gdal.GDT_CFloat16, None, None),
      ('<c8', gdal.GDT_CFloat32, None, None),
      ('>c8', gdal.GDT_CFloat32, None, None),
      ('<c16', gdal.GDT_CFloat64, None, None),
      ('>c16', gdal.GDT_CFloat64, None, None),
  ])
  def test_zarr_basic(self, dtype: str, gdaltype, fill_value, nodata_value):
    # TODO: b/389359730 - Make use_optimized_code_paths be True and False.
    use_optimized_code_paths = True

    structtype = _gdal_data_type_to_array_type[gdaltype]

    j = {
        'chunks': [2, 3],
        'compressor': None,
        'dtype': dtype,
        'fill_value': fill_value,
        'filters': None,
        'order': 'C',
        'shape': [5, 4],
        'zarr_format': 2,
    }

    dirname = pathlib.Path('/vsimem/test_basic.zarr')
    gdal.Mkdir(dirname, 0o755)
    gdal.FileFromMemBuffer(dirname / '.zarray', json.dumps(j))

    if gdaltype not in (
        gdal.GDT_CFloat16,
        gdal.GDT_CFloat32,
        gdal.GDT_CFloat64,
    ):
      tile_0_0_data = struct.pack(dtype[0] + (structtype * 6), 1, 2, 3, 5, 6, 7)
      tile_0_1_data = struct.pack(dtype[0] + (structtype * 6), 4, 0, 0, 8, 0, 0)
    else:
      tile_0_0_data = struct.pack(
          dtype[0] + (structtype * 12), 1, 11, 2, 0, 3, 0, 5, 0, 6, 0, 7, 0
      )
      tile_0_1_data = struct.pack(
          dtype[0] + (structtype * 12), 4, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0
      )
    gdal.FileFromMemBuffer(dirname / '0.0', tile_0_0_data)
    gdal.FileFromMemBuffer(dirname / '0.1', tile_0_1_data)

    with gdal.config_option(
        'GDAL_ZARR_USE_OPTIMIZED_CODE_PATHS',
        'YES' if use_optimized_code_paths else 'NO',
    ):
      ds = gdal.OpenEx(dirname, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
    self.assertEqual(ar.GetDimensionCount(), 2)
    self.assertEqual(
        [ar.GetDimensions()[i].GetSize() for i in range(2)], [5, 4]
    )
    self.assertEqual(ar.GetBlockSize(), [2, 3])
    if nodata_value is not None and math.isnan(nodata_value):
      self.assertTrue(math.isnan(ar.GetNoDataValue()))
    else:
      self.assertEqual(ar.GetNoDataValue(), nodata_value)

    self.assertIsNone(ar.GetOffset())
    self.assertIsNone(ar.GetScale())
    self.assertEmpty(ar.GetUnit())

    # Check reading one single value
    result = ar[1, 2].Read(
        buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64)
    )
    self.assertEqual(result, struct.pack('d' * 1, 7))

    structtype_read = structtype

    # Read block 0,0
    if gdaltype not in (
        gdal.GDT_CFloat16,
        gdal.GDT_CFloat32,
        gdal.GDT_CFloat64,
    ):
      self.assertEqual(
          ar[0:2, 0:3].Read(
              buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64)
          ),
          struct.pack('d' * 6, 1, 2, 3, 5, 6, 7),
      )
      expect = (1, 2, 3, 5, 6, 7)
      self.assertEqual(
          struct.unpack(structtype_read * 6, ar[0:2, 0:3].Read()), expect
      )
    else:
      self.assertEqual(
          ar[0:2, 0:3].Read(
              buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_CFloat64)
          ),
          struct.pack('d' * 12, 1, 11, 2, 0, 3, 0, 5, 0, 6, 0, 7, 0),
      )
      expect = (1, 11, 2, 0, 3, 0, 5, 0, 6, 0, 7, 0)
      self.assertEqual(
          struct.unpack(structtype * 12, ar[0:2, 0:3].Read()), expect
      )

    # Read block 0,1
    self.assertEqual(
        ar[0:2, 3:4].Read(
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64)
        ),
        struct.pack('d' * 2, 4, 8),
    )

    # Read block 1,1 (missing)
    nv = nodata_value if nodata_value else 0
    self.assertEqual(
        ar[2:4, 3:4].Read(
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64)
        ),
        struct.pack('d' * 2, nv, nv),
    )

    # Read whole raster
    self.assertEqual(
        ar.Read(buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64)),
        struct.pack(
            'd' * 20,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
            nv,
        ),
    )

    if gdaltype not in (
        gdal.GDT_CFloat16,
        gdal.GDT_CFloat32,
        gdal.GDT_CFloat64,
    ):
      self.assertEqual(
          ar.Read(),
          array.array(
              structtype_read,
              [
                  1,
                  2,
                  3,
                  4,
                  5,
                  6,
                  7,
                  8,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
                  nv,
              ],
          ),
      )
    else:
      self.assertEqual(
          ar.Read(),
          array.array(
              structtype,
              [
                  1,
                  11,
                  2,
                  0,
                  3,
                  0,
                  4,
                  0,
                  5,
                  0,
                  6,
                  0,
                  7,
                  0,
                  8,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
                  nv,
                  0,
              ],
          ),
      )
    # Read with negative steps
    self.assertEqual(
        ar.Read(
            array_start_idx=[2, 1],
            count=[2, 2],
            array_step=[-1, -1],
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
        ),
        struct.pack('d' * 4, nv, nv, 6, 5),
    )

    # array_step > 2
    self.assertEqual(
        ar.Read(
            array_start_idx=[0, 0],
            count=[1, 2],
            array_step=[0, 2],
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
        ),
        struct.pack('d' * 2, 1, 3),
    )

    self.assertEqual(
        ar.Read(
            array_start_idx=[0, 0],
            count=[3, 1],
            array_step=[2, 0],
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
        ),
        struct.pack('d' * 3, 1, nv, nv),
    )

    self.assertEqual(
        ar.Read(
            array_start_idx=[0, 1],
            count=[1, 2],
            array_step=[0, 2],
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
        ),
        struct.pack('d' * 2, 2, 4),
    )

    self.assertEqual(
        ar.Read(
            array_start_idx=[0, 0],
            count=[1, 2],
            array_step=[0, 3],
            buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
        ),
        struct.pack('d' * 2, 1, 4),
    )

    gdal.RmdirRecursive(dirname)

  @parameterized.parameters([
      (base64.b64encode(b'xyz').decode('utf-8'), ['abc', 'xyz']),
      (None, ['abc', None]),
  ])
  def test_zarr_string(self, fill_value, expected_read_data):
    j = {
        'chunks': [1],
        'compressor': None,
        'dtype': '|S3',
        'fill_value': fill_value,
        'filters': [],
        'order': 'C',
        'shape': [2],
        'zarr_format': 2,
    }

    filename = pathlib.Path('/vsimem/test_string.zarr')
    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))
    gdal.FileFromMemBuffer(filename / '0', b'abc')

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), expected_read_data)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      (None, None),
      ('chunks', 'chunks missing or not an array'),
      ('compressor', 'compressor missing'),
      ('dtype', 'dtype missing'),
      ('fill_value', None),
      ('filters', 'filters missing'),
      ('order', 'Invalid value for order'),
      ('shape', 'shape missing or not an array'),
      ('zarr_format', 'Invalid value for zarr_format'),
  ])
  def test_zarr_invalid_json_remove_member(self, member, message):
    j = {
        'chunks': [2, 3],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [5, 4],
        'zarr_format': 2,
    }

    if member:
      del j[member]

    filename = pathlib.Path(f'/vsimem/invalid_json_remove_member_{member}.zarr')
    gdal.Mkdir(filename, 0o755)

    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    if message:
      with self.assertRaisesRegex(RuntimeError, message):
        gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    else:
      if member == 'fill_value':
        # Warning 1: fill_value missing.
        with gdal.quiet_warnings():
          with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
            self.assertIsNotNone(ds)
      else:
        with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
          self.assertIsNotNone(ds)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ({'chunks': None}, 'chunks missing or not an array'),
      ({'chunks': 'invalid'}, 'chunks missing or not an array'),
      ({'chunks': [2]}, 'shape and chunks arrays are of different size'),
      ({'chunks': [2, 0]}, 'Invalid content for chunks'),
      ({'shape': None}, 'shape missing or not an array'),
      ({'shape': 'invalid'}, 'shape missing or not an array'),
      ({'shape': [5]}, 'shape and chunks arrays are of different size'),
      ({'shape': [5, 0]}, 'Invalid content for shape'),
      (
          {'chunks': [1 << 40, 1 << 40], 'shape': [1 << 40, 1 << 40]},
          'Too large chunks',
      ),
      (
          {'shape': [1 << 30, 1 << 30, 1 << 30], 'chunks': [1, 1, 1]},
          r'Array invalid_json_wrong_values has more than 2\^64 tiles',
      ),
      ({'dtype': None}, 'Invalid or unsupported format for dtype'),
      ({'dtype': 1}, 'Invalid or unsupported format for dtype: 1'),
      ({'dtype': ''}, 'Invalid or unsupported format for dtype: '),
      ({'dtype': '!'}, 'Invalid or unsupported format for dtype: !'),
      ({'dtype': '!b'}, 'Invalid or unsupported format for dtype: !b'),
      ({'dtype': '<u16'}, 'Invalid or unsupported format for dtype: <u16'),
      ({'dtype': '<u0'}, 'Invalid or unsupported format for dtype: <u0'),
      (
          {'dtype': '<u10000'},
          'Invalid or unsupported format for dtype: <u10000',
      ),
      ({'fill_value': []}, 'Invalid fill_value'),
      ({'fill_value': 'x'}, 'Invalid fill_value'),
      ({'fill_value': 'NaN'}, 'Invalid fill_value'),
      ({'dtype': '!S1', 'fill_value': 0}, 'Invalid fill_value'),
      ({'order': None}, 'Invalid value for order'),
      ({'order': 'invalid'}, 'Invalid value for order'),
      ({'compressor': 'invalid'}, 'Invalid compressor'),
      ({'compressor': {}}, 'Missing compressor id'),
      ({'compressor': {'id': 'invalid'}}, 'Decompressor invalid not handled'),
      ({'filters': 'invalid'}, 'Invalid filters'),
      ({'filters': {}}, 'Invalid filters'),
      ({'filters': [{'missing_id': True}]}, 'Missing filter id'),
      ({'zarr_format': None}, 'Invalid value for zarr_format'),
      ({'zarr_format': 1}, 'Invalid value for zarr_format'),
  ])
  def test_zarr_invalid_json_wrong_values(self, dict_update, message):
    j = {
        'chunks': [2, 3],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [5, 4],
        'zarr_format': 2,
    }

    j.update(dict_update)

    filename = pathlib.Path('/vsimem/invalid_json_wrong_values.zarr')
    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    if message:
      with self.assertRaisesRegex(RuntimeError, message):
        gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    else:
      with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
        self.assertIsNotNone(ds)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      ['blosc', 'gzip', 'lz4', 'lzma', 'lzma_with_filters', 'zlib', 'zstd']
  )
  def test_zarr_read_compression_methods(self, compressor):
    filename = self.getTestFilePath(f'{compressor}.zarr')
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      self.assertIsNotNone(rg)
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertIsNotNone(ar)
      self.assertEqual(ar.Read(), array.array('b', [1, 2]))

      result = json.loads(ar.GetStructuralInfo()['COMPRESSOR'])['id']
      with_filters = '_with_filters'
      if compressor.endswith(with_filters):
        compressor = compressor.removesuffix(with_filters)
      self.assertEqual(result, compressor)

  def test_zarr_v3_read_compression_methods(self):
    filename = self.getTestFilePath('gzip.zr3')
    compressor = 'gzip'
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('b', [1, 2]))
      result = json.loads(ar.GetStructuralInfo()['COMPRESSOR'])['name']
      self.assertEqual(result, compressor)

  def test_zarr_read_shuffle_filter(self):
    filename = self.getTestFilePath('shuffle.zarr')
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('h', [1, 2]))
      result = json.loads(ar.GetStructuralInfo()['FILTERS'])
      self.assertEqual(result, [{'elementsize': 2, 'id': 'shuffle'}])

  def test_zarr_read_shuffle_filter_update(self):
    src_dirname = self.getTestFilePath('shuffle.zarr')
    data_zarray = (pathlib.Path(src_dirname) / '.zarray').read_bytes()
    data_0 = (pathlib.Path(src_dirname) / '0').read_bytes()

    dirname = pathlib.Path('/vsimem/shuffle_update.zarr')

    gdal.Mkdir(dirname, 0o755)
    gdal.FileFromMemBuffer(dirname / '.zarray', data_zarray)
    gdal.FileFromMemBuffer(dirname / '0', data_0)

    open_options = gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE
    with gdal.OpenEx(dirname, open_options) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      ar.Write([3, 4])

    with gdal.OpenEx(dirname, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('h', [3, 4]))

    gdal.RmdirRecursive(dirname)

  def test_zarr_read_shuffle_quantize(self):
    def generate_values():
      ratios = [
          0,
          1 / 8,
          3 / 16,
          5 / 16,
          3 / 8,
          4 / 8,
          5 / 8,
          11 / 16,
          13 / 16,
          7 / 8,
      ]
      values = []
      for i in range(10):
        for ratio in ratios:
          values.append(i + ratio)
      return values

    filename = self.getTestFilePath('quantize.zarr')
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('d', generate_values()))

  @parameterized.parameters(['u1', 'u2', 'u4', 'u8'])
  def test_zarr_read_fortran_order(self, name):
    filename = self.getTestFilePath(f'order_f_{name}.zarr')

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(
          ar.Read(buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Byte)),
          array.array('b', [i for i in range(16)]),
      )

  def test_zarr_read_fortran_order_string(self):
    filename = self.getTestFilePath('order_f_s3.zarr')

    expect = [
        '000',
        '111',
        '222',
        '333',
        '444',
        '555',
        '666',
        '777',
        '888',
        '999',
        'AAA',
        'BBB',
        'CCC',
        'DDD',
        'EEE',
        'FFF',
    ]

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), expect)

  def test_zarr_read_fortran_order_3d(self):
    filename = self.getTestFilePath('order_f_u1_3d.zarr')
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      result = ar.Read(
          buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      expect = array.array('b', [i for i in range(2 * 3 * 4)])
      self.assertEqual(result, expect)

  def test_zarr_read_compound_well_aligned(self):
    filename = self.getTestFilePath('compound_well_aligned.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)

    rg = ds.GetRootGroup()
    ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])

    dt = ar.GetDataType()
    self.assertEqual(dt.GetSize(), 4)
    comps = dt.GetComponents()
    self.assertLen(comps, 2)
    self.assertEqual(comps[0].GetName(), 'a')
    self.assertEqual(comps[0].GetOffset(), 0)
    self.assertEqual(comps[0].GetType().GetNumericDataType(), gdal.GDT_UInt16)
    self.assertEqual(comps[1].GetName(), 'b')
    self.assertEqual(comps[1].GetOffset(), 2)
    self.assertEqual(comps[1].GetType().GetNumericDataType(), gdal.GDT_UInt16)

    self.assertEqual(ar['a'].Read(), array.array('H', [1000, 4000, 0]))
    self.assertEqual(ar['b'].Read(), array.array('H', [3000, 5000, 0]))

    j = gdal.MultiDimInfo(ds, detailed=True)
    result = j['arrays']['compound_well_aligned']['values']
    self.assertEqual(
        result,
        [
            {'a': 1000, 'b': 3000},
            {'a': 4000, 'b': 5000},
            {'a': 0, 'b': 0},
        ],
    )

  def test_zarr_read_compound_not_aligned(self):
    filename = self.getTestFilePath('compound_not_aligned.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])

    dt = ar.GetDataType()
    self.assertEqual(dt.GetSize(), 6)
    comps = dt.GetComponents()
    self.assertLen(comps, 3)
    self.assertEqual(comps[0].GetName(), 'a')
    self.assertEqual(comps[0].GetOffset(), 0)
    self.assertEqual(comps[0].GetType().GetNumericDataType(), gdal.GDT_UInt16)
    self.assertEqual(comps[1].GetName(), 'b')
    self.assertEqual(comps[1].GetOffset(), 2)
    self.assertEqual(comps[1].GetType().GetNumericDataType(), gdal.GDT_Byte)
    self.assertEqual(comps[2].GetName(), 'c')
    self.assertEqual(comps[2].GetOffset(), 4)
    self.assertEqual(comps[2].GetType().GetNumericDataType(), gdal.GDT_UInt16)

    self.assertEqual(ar['a'].Read(), array.array('H', [1000, 4000, 0]))
    self.assertEqual(ar['b'].Read(), array.array('B', [2, 4, 0]))
    self.assertEqual(ar['c'].Read(), array.array('H', [3000, 5000, 0]))

    j = gdal.MultiDimInfo(ds, detailed=True)
    result = j['arrays']['compound_not_aligned']['values']
    self.assertEqual(
        result,
        [
            {'a': 1000, 'b': 2, 'c': 3000},
            {'a': 4000, 'b': 4, 'c': 5000},
            {'a': 0, 'b': 0, 'c': 0},
        ],
    )

  def test_zarr_read_compound_complex(self):
    filename = self.getTestFilePath('compound_complex.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])

    dt = ar.GetDataType()
    self.assertEqual(dt.GetSize(), 24)
    comps = dt.GetComponents()
    self.assertLen(comps, 4)
    self.assertEqual(comps[0].GetName(), 'a')
    self.assertEqual(comps[0].GetOffset(), 0)
    self.assertEqual(comps[0].GetType().GetNumericDataType(), gdal.GDT_Byte)
    self.assertEqual(comps[1].GetName(), 'b')
    self.assertEqual(comps[1].GetOffset(), 2)
    self.assertEqual(comps[1].GetType().GetClass(), gdal.GEDTC_COMPOUND)
    # Last one is padding.
    self.assertEqual(comps[1].GetType().GetSize(), 1 + 1 + 2 + 1 + 1)

    subcomps = comps[1].GetType().GetComponents()
    self.assertLen(subcomps, 4)

    self.assertEqual(comps[2].GetName(), 'c')
    self.assertEqual(comps[2].GetOffset(), 8)
    self.assertEqual(comps[2].GetType().GetClass(), gdal.GEDTC_STRING)
    self.assertEqual(comps[3].GetName(), 'd')
    self.assertEqual(comps[3].GetOffset(), 16)
    self.assertEqual(comps[3].GetType().GetNumericDataType(), gdal.GDT_Int8)

    j = gdal.MultiDimInfo(ds, detailed=True)
    result = j['arrays']['compound_complex']['values']
    self.assertEqual(
        result,
        [
            {
                'a': 1,
                'b': {'b1': 2, 'b2': 3, 'b3': 1000, 'b5': 4},
                'c': 'AAA',
                'd': -1,
            },
            {
                'a': 2,
                'b': {'b1': 255, 'b2': 254, 'b3': 65534, 'b5': 253},
                'c': 'ZZ',
                'd': -2,
            },
        ],
    )

  def test_zarr_read_array_attributes(self):
    filename = self.getTestFilePath('array_attrs.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)

    j = gdal.MultiDimInfo(ds)
    result = j['arrays']['array_attrs']['attributes']
    self.assertEqual(
        result,
        {
            'bool': True,
            'double': 1.5,
            'doublearray': [1.5, 2.5],
            'int': 1,
            'intarray': [1, 2],
            'int64': 1234567890123,
            'int64array': [1234567890123, -1234567890123],
            'intdoublearray': [1, 2.5],
            'mixedstrintarray': ['foo', 1],
            'null': '',
            'obj': {},
            'str': 'foo',
            'strarray': ['foo', 'bar'],
        },
    )

  @parameterized.parameters(['projjson', 'wkt', 'url'])
  def test_zarr_read_crs(self, crs_member):
    zarray = {
        'chunks': [2, 3],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [5, 4],
        'zarr_format': 2,
    }

    zattrs_all = {
        '_CRS': {
            'projjson': {
                '$schema': 'https://proj.org/schemas/v0.2/projjson.schema.json',
                'type': 'GeographicCRS',
                'name': 'WGS 84',
                'datum_ensemble': {
                    'name': 'World Geodetic System 1984 ensemble',
                    'members': [
                        {
                            'name': 'World Geodetic System 1984 (Transit)',
                            'id': {'authority': 'EPSG', 'code': 1166},
                        },
                        {
                            'name': 'World Geodetic System 1984 (G730)',
                            'id': {'authority': 'EPSG', 'code': 1152},
                        },
                        {
                            'name': 'World Geodetic System 1984 (G873)',
                            'id': {'authority': 'EPSG', 'code': 1153},
                        },
                        {
                            'name': 'World Geodetic System 1984 (G1150)',
                            'id': {'authority': 'EPSG', 'code': 1154},
                        },
                        {
                            'name': 'World Geodetic System 1984 (G1674)',
                            'id': {'authority': 'EPSG', 'code': 1155},
                        },
                        {
                            'name': 'World Geodetic System 1984 (G1762)',
                            'id': {'authority': 'EPSG', 'code': 1156},
                        },
                    ],
                    'ellipsoid': {
                        'name': 'WGS 84',
                        'semi_major_axis': 6378137,
                        'inverse_flattening': 298.257223563,
                    },
                    'accuracy': '2.0',
                    'id': {'authority': 'EPSG', 'code': 6326},
                },
                'coordinate_system': {
                    'subtype': 'ellipsoidal',
                    'axis': [
                        {
                            'name': 'Geodetic latitude',
                            'abbreviation': 'Lat',
                            'direction': 'north',
                            'unit': 'degree',
                        },
                        {
                            'name': 'Geodetic longitude',
                            'abbreviation': 'Lon',
                            'direction': 'east',
                            'unit': 'degree',
                        },
                    ],
                },
                'scope': 'Horizontal component of 3D system.',
                'area': 'World.',
                'bbox': {
                    'south_latitude': -90,
                    'west_longitude': -180,
                    'north_latitude': 90,
                    'east_longitude': 180,
                },
                'id': {'authority': 'EPSG', 'code': 4326},
            },
            'wkt': (
                'GEOGCRS["WGS 84",ENSEMBLE["World Geodetic System 1984'
                ' ensemble",'
                'MEMBER["World Geodetic System 1984 (Transit)"],'
                'MEMBER["World Geodetic System 1984 (G730)"],'
                'MEMBER["World Geodetic System 1984 (G873)"],'
                'MEMBER["World Geodetic System 1984 (G1150)"],'
                'MEMBER["World Geodetic System 1984 (G1674)"],'
                'MEMBER["World Geodetic System 1984 (G1762)"],'
                'ELLIPSOID["WGS'
                ' 84",6378137,298.257223563,LENGTHUNIT["metre",1]],'
                'ENSEMBLEACCURACY[2.0]],'
                'PRIMEM["Greenwich",0,ANGLEUNIT["degree",0.0174532925199433]],'
                'CS[ellipsoidal,2],'
                'AXIS["geodetic latitude'
                ' (Lat)",north,ORDER[1],ANGLEUNIT["degree",0.0174532925199433]],'
                'AXIS["geodetic longitude'
                ' (Lon)",east,ORDER[2],ANGLEUNIT["degree",0.0174532925199433]],'
                'USAGE[SCOPE["Horizontal component of 3D'
                ' system."],AREA["World."],BBOX[-90,-180,90,180]],'
                'ID["EPSG",4326]]'
            ),
            'url': 'http://www.opengis.net/def/crs/EPSG/0/4326',
        }
    }

    zattrs = {'_CRS': {crs_member: zattrs_all['_CRS'][crs_member]}}

    filename = pathlib.Path(f'/vsimem/test_zarr_read_crs_{crs_member}.zarr')
    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(zarray))
    gdal.FileFromMemBuffer(filename / '.zattrs', json.dumps(zattrs))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      self.assertIsNotNone(rg)
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      srs = ar.GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '4326')
      # Mapping is 1, 2 since the slowest varying axis in multidim
      # mode is the lines, which matches latitude as the first axis of the CRS.
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [1, 2])
      self.assertEmpty(ar.GetAttributes())

    # Open as classic CRS
    with gdal.Open(filename) as ds:
      srs = ds.GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '4326')
      # Inverted mapping in classic raster mode compared to multidim mode,
      # because the first "axis" in our data model is columns.
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [2, 1])

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([True, False])
  def test_zarr_read_group(self, use_get_names):
    filename = self.getTestFilePath('group.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    self.assertEqual(rg.GetName(), '/')
    self.assertEqual(rg.GetFullName(), '/')
    if use_get_names:
      self.assertEqual(rg.GetGroupNames(), ['foo'])
    self.assertLen(rg.GetAttributes(), 1)
    self.assertIsNotNone(rg.GetAttribute('key'))
    subgroup = rg.OpenGroup('foo')
    self.assertIsNotNone(subgroup)
    with no_gdal_exceptions():
      self.assertIsNone(rg.OpenGroup('not_existing'))
    self.assertEqual(subgroup.GetName(), 'foo')
    self.assertEqual(subgroup.GetFullName(), '/foo')
    self.assertEmpty(rg.GetMDArrayNames())
    if use_get_names:
      self.assertEqual(subgroup.GetGroupNames(), ['bar'])
    self.assertEmpty(subgroup.GetAttributes())
    subsubgroup = subgroup.OpenGroup('bar')
    self.assertEqual(subgroup.GetGroupNames(), ['bar'])
    self.assertEqual(subsubgroup.GetName(), 'bar')
    self.assertEqual(subsubgroup.GetFullName(), '/foo/bar')
    if use_get_names:
      self.assertEqual(subsubgroup.GetMDArrayNames(), ['baz'])
    ar = subsubgroup.OpenMDArray('baz')
    self.assertEqual(subsubgroup.GetMDArrayNames(), ['baz'])
    self.assertEqual(ar.Read(), array.array('i', [1]))
    with no_gdal_exceptions():
      self.assertIsNone(subsubgroup.OpenMDArray('not_existing'))

  def test_zarr_read_group_with_zmetadata(self):
    filename = self.getTestFilePath('group_with_zmetadata.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    self.assertEqual(rg.GetName(), '/')
    self.assertEqual(rg.GetFullName(), '/')
    self.assertEqual(rg.GetGroupNames(), ['foo'])
    self.assertLen(rg.GetAttributes(), 1)
    self.assertIsNotNone(rg.GetAttribute('key'))
    subgroup = rg.OpenGroup('foo')
    with no_gdal_exceptions():
      self.assertIsNone(rg.OpenGroup('not_existing'))
    self.assertEqual(subgroup.GetName(), 'foo')
    self.assertEqual(subgroup.GetFullName(), '/foo')
    self.assertEmpty(rg.GetMDArrayNames())
    self.assertEqual(subgroup.GetGroupNames(), ['bar'])
    self.assertEmpty(subgroup.GetAttributes())
    subsubgroup = subgroup.OpenGroup('bar')
    self.assertEqual(subgroup.GetGroupNames(), ['bar'])
    self.assertEqual(subsubgroup.GetName(), 'bar')
    self.assertEqual(subsubgroup.GetFullName(), '/foo/bar')
    self.assertEqual(subsubgroup.GetMDArrayNames(), ['baz'])
    self.assertIsNotNone(subsubgroup.GetAttribute('foo'))
    ar = subsubgroup.OpenMDArray('baz')
    self.assertEqual(subsubgroup.GetMDArrayNames(), ['baz'])
    self.assertEqual(ar.Read(), array.array('i', [1]))
    self.assertIsNotNone(ar.GetAttribute('bar'))
    with no_gdal_exceptions():
      self.assertIsNone(subsubgroup.OpenMDArray('not_existing'))

  @parameterized.parameters([
      (True, 'array_dimensions.zarr'),
      (False, 'array_dimensions.zarr'),
      (True, 'array_dimensions_upper_level.zarr'),
      (False, 'array_dimensions_upper_level.zarr'),
      (False, 'array_dimensions_upper_level.zarr/subgroup/var'),
  ])
  def test_zarr_read_ARRAY_DIMENSIONS(self, use_zmetadata, filename):
    filename = self.getTestFilePath(filename)

    with gdal.OpenEx(
        filename,
        gdal.OF_MULTIDIM_RASTER,
        open_options=['USE_ZMETADATA=' + str(use_zmetadata)],
    ) as ds:
      rg = ds.GetRootGroup()

      # if 'array_dimensions_upper_level.zarr' not in filename:
      if not filename.endswith('array_dimensions_upper_level.zarr'):
        ar = rg.OpenMDArray('var')
      else:
        ar = rg.OpenGroup('subgroup').OpenMDArray('var')

      dims = ar.GetDimensions()
      self.assertLen(dims, 2)

      self.assertEqual(dims[0].GetName(), 'lat')
      self.assertIsNotNone(dims[0].GetIndexingVariable())
      self.assertEqual(dims[0].GetIndexingVariable().GetName(), 'lat')
      self.assertEqual(dims[0].GetType(), gdal.DIM_TYPE_HORIZONTAL_Y)
      self.assertEqual(dims[0].GetDirection(), 'NORTH')
      self.assertEqual(dims[1].GetName(), 'lon')
      self.assertIsNotNone(dims[1].GetIndexingVariable())
      self.assertEqual(dims[1].GetIndexingVariable().GetName(), 'lon')
      self.assertEqual(dims[1].GetType(), gdal.DIM_TYPE_HORIZONTAL_X)
      self.assertEqual(dims[1].GetDirection(), 'EAST')
      self.assertLen(rg.GetDimensions(), 2)

    with gdal.OpenEx(
        filename,
        gdal.OF_MULTIDIM_RASTER,
        open_options=['USE_ZMETADATA=' + str(use_zmetadata)],
    ) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('lat')
      dims = ar.GetDimensions()
      self.assertLen(dims, 1)
      self.assertEqual(dims[0].GetName(), 'lat')
      self.assertEqual(dims[0].GetIndexingVariable().GetName(), 'lat')
      self.assertEqual(dims[0].GetType(), gdal.DIM_TYPE_HORIZONTAL_Y)
      self.assertLen(rg.GetDimensions(), 2)

    with gdal.OpenEx(
        filename,
        gdal.OF_MULTIDIM_RASTER,
        open_options=['USE_ZMETADATA=' + str(use_zmetadata)],
    ) as ds:
      rg = ds.GetRootGroup()
      self.assertLen(rg.GetDimensions(), 2)

  @parameterized.parameters([
      [True, 'test.zr3'],
      [False, 'test.zr3'],
  ])
  def test_zarr_read_v3(self, use_get_names, filename):
    filename = self.getTestFilePath(filename)
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    self.assertEqual(rg.GetName(), '/')
    self.assertEqual(rg.GetFullName(), '/')
    if use_get_names:
      self.assertEqual(rg.GetGroupNames(), ['marvin'])
    self.assertLen(rg.GetAttributes(), 1)
    subgroup = rg.OpenGroup('marvin')
    with no_gdal_exceptions():
      self.assertIsNone(rg.OpenGroup('not_existing'))
    self.assertEqual(subgroup.GetName(), 'marvin')
    self.assertEqual(subgroup.GetFullName(), '/marvin')
    if use_get_names:
      self.assertEqual(rg.GetMDArrayNames(), ['ar'])

    ar = rg.OpenMDArray('ar')
    self.assertEqual(ar.Read(), array.array('b', [1, 2]))

    if use_get_names:
      self.assertEqual(subgroup.GetGroupNames(), ['paranoid'])
    self.assertLen(subgroup.GetAttributes(), 1)

    subsubgroup = subgroup.OpenGroup('paranoid')
    self.assertEqual(subsubgroup.GetName(), 'paranoid')
    self.assertEqual(subsubgroup.GetFullName(), '/marvin/paranoid')

    if use_get_names:
      self.assertEqual(subgroup.GetMDArrayNames(), ['android'])
    ar = subgroup.OpenMDArray('android')
    self.assertEqual(ar.Read(), array.array('b', [1] * 4 * 5))
    with no_gdal_exceptions():
      self.assertIsNone(subgroup.OpenMDArray('not_existing'))

  @parameterized.parameters(['le', 'be'])
  def test_zarr_read_half_float(self, endianness):
    filename = self.getTestFilePath(f'f2_{endianness}.zarr')
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
    self.assertEqual(ar.Read(), array.array('f', [1.5, math.nan]))

  def test_zarr_read_mdim_zarr_non_existing(self):
    with self.assertRaisesRegex(RuntimeError, 'No such file or directory'):
      gdal.OpenEx('ZARR:nope/does_not_exist.zarr', gdal.OF_MULTIDIM_RASTER)

    message = 'The filename should likely be prefixed with /vsicurl/'
    with self.assertRaisesRegex(RuntimeError, message):
      path = 'ZARR:"https://example.org/not_existing.zarr"'
      gdal.OpenEx(path, gdal.OF_MULTIDIM_RASTER)

    message = 'There is likely a quoting error of the whole connection string'
    with self.assertRaisesRegex(RuntimeError, message):
      path = 'ZARR:https://example.org/not_existing.zarr'
      gdal.OpenEx(path, gdal.OF_MULTIDIM_RASTER)

    # Same message as last.
    with self.assertRaisesRegex(RuntimeError, message):
      path = 'ZARR:/vsicurl/https://example.org/not_existing.zarr'
      gdal.OpenEx(path, gdal.OF_MULTIDIM_RASTER)

  def test_zarr_read_classic(self):
    filename = self.getTestFilePath('zlib.zarr')
    with gdal.Open(filename) as ds:
      self.assertEmpty(ds.GetSubDatasets())
      self.assertEqual(ds.ReadRaster(), array.array('b', [1, 2]))

    with gdal.Open(f'ZARR:{filename}') as ds:
      self.assertEmpty(ds.GetSubDatasets())
      self.assertEqual(ds.ReadRaster(), array.array('b', [1, 2]))

    message = 'No such file or directory'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.Open(f'ZARR:"{filename}":/not_existing')

    message = 'Unexpected extra indices'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.Open(f'ZARR:"{filename}":/zlib:0')

    with gdal.Open(f'ZARR:"{filename}":/zlib') as ds:
      self.assertEmpty(ds.GetSubDatasets())
      self.assertEqual(ds.ReadRaster(), array.array('b', [1, 2]))

    filename = self.getTestFilePath('order_f_u1_3d.zarr')
    with gdal.OpenEx(filename, open_options=['MULTIBAND=NO']) as ds:
      subds = ds.GetSubDatasets()
      self.assertLen(subds, 2)
      ds1 = gdal.Open(subds[0][0])
      self.assertEqual(
          ds1.ReadRaster(), array.array('b', [i for i in range(12)])
      )
      ds2 = gdal.Open(subds[1][0])
      self.assertEqual(
          ds2.ReadRaster(), array.array('b', [12 + i for i in range(12)])
      )

      message = 'Wrong number of indices of extra dimensions'
      with self.assertRaisesRegex(RuntimeError, message):
        gdal.Open(subds[0][0] + ':0')

    message = 'Indices of extra dimensions must be specified'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.OpenEx(
          f'ZARR:{filename}:/order_f_u1_3d', open_options=['MULTIBAND=NO']
      )

    with self.assertRaisesRegex(RuntimeError, 'Index 2 is out of bounds'):
      gdal.Open(f'ZARR:{filename}:/order_f_u1_3d:2')

  def test_zarr_read_classic_2d(self):
    src_filepath = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    src_ds = gdal.Open(src_filepath)

    filepath = pathlib.Path('/vsimem/test_zarr_read_classic_2d.zarr')
    with self.driver.CreateCopy(filepath, src_ds, strict=False) as ds:
      self.assertEmpty(ds.GetSubDatasets())
      srs = ds.GetSpatialRef()
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [1, 2])

    gdal.RmdirRecursive(filepath)

  def test_zarr_read_classic_2d_with_unrelated_auxiliary_1D_arrays(self):
    filepath = pathlib.Path('/vsimem/test_with_unrelated_aux_1D_arrays.zarr')

    with self.driver.CreateMultiDimensional(filepath) as ds:
      rg = ds.GetRootGroup()
      self.assertEqual(rg.GetName(), '/')

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim1 = rg.CreateDimension('dim1', None, None, 3)

      dt = gdal.ExtendedDataType.Create(gdal.GDT_Float64)
      rg.CreateMDArray('main_array', [dim0, dim1], dt)
      rg.CreateMDArray('x', [dim0], dt)
      rg.CreateMDArray('y', [dim1], dt)

    with gdal.Open(filepath) as ds:
      self.assertEqual(ds.RasterYSize, 2)
      self.assertEqual(ds.RasterXSize, 3)
      expect = [
          (f'ZARR:"{filepath}":/main_array', '[2x3] /main_array (Float64)')
      ]
      self.assertEqual(ds.GetSubDatasets(), expect)

    with gdal.OpenEx(filepath, open_options=['LIST_ALL_ARRAYS=YES']) as ds:
      expect = [
          (f'ZARR:"{filepath}":/main_array', '[2x3] /main_array (Float64)'),
          (f'ZARR:"{filepath}":/x', '[2] /x (Float64)'),
          (f'ZARR:"{filepath}":/y', '[3] /y (Float64)'),
      ]
      self.assertEqual(ds.GetSubDatasets(), expect)

    gdal.RmdirRecursive(filepath)

  def test_zarr_read_classic_3d_multiband(self):
    filename = self.getTestFilePath('order_f_u1_3d.zarr')

    with gdal.OpenEx(filename, open_options=['MULTIBAND=YES']) as ds:
      self.assertEqual(ds.RasterXSize, 4)
      self.assertEqual(ds.RasterYSize, 3)
      self.assertEqual(ds.RasterCount, 2)
      self.assertEmpty(ds.GetSubDatasets())

      expect = array.array('b', [i for i in range(12)])
      self.assertEqual(ds.GetRasterBand(1).ReadRaster(), expect)

      expect = array.array('b', [12 + i for i in range(12)])
      self.assertEqual(ds.GetRasterBand(2).ReadRaster(), expect)

    open_options = ['MULTIBAND=YES', 'DIM_X=dim1', 'DIM_Y=dim2']
    with gdal.OpenEx(filename, open_options=open_options) as ds:
      self.assertEqual(ds.RasterXSize, 3)
      self.assertEqual(ds.RasterYSize, 4)
      self.assertEqual(ds.RasterCount, 2)
      self.assertEmpty(ds.GetSubDatasets())

      expect = array.array(
          'b', [(x + 12) for x in [0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11]]
      )
      self.assertEqual(ds.GetRasterBand(2).ReadRaster(), expect)

    open_options = ['MULTIBAND=YES', 'DIM_X=1', 'DIM_Y=2']
    with gdal.OpenEx(filename, open_options=open_options) as ds:
      self.assertEqual(ds.RasterXSize, 3)
      self.assertEqual(ds.RasterYSize, 4)
      self.assertEqual(ds.RasterCount, 2)
      self.assertEmpty(ds.GetSubDatasets())

      expect = array.array(
          'b', [(x + 12) for x in [0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11]]
      )
      self.assertEqual(ds.GetRasterBand(2).ReadRaster(), expect)

    open_options = ['MULTIBAND=YES', 'DIM_X=3']
    message = 'Invalid iXDim and/or iYDim'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.OpenEx(filename, open_options=open_options)

    open_options = ['MULTIBAND=YES', 'DIM_Y=3']
    message = 'Invalid iXDim and/or iYDim'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.OpenEx(filename, open_options=open_options)

    open_options = ['MULTIBAND=YES', 'DIM_X=not_found']
    self.assertIsNotNone(gdal.OpenEx(filename, open_options=open_options))

    open_options = ['MULTIBAND=YES', 'DIM_Y=not_found']
    self.assertIsNotNone(gdal.OpenEx(filename, open_options=open_options))

  def test_zarr_read_classic_too_many_samples_3d(self):
    j = {
        'chunks': [65537, 2, 1],
        'compressor': None,
        'dtype': '!u1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [65537, 2, 1],
        'zarr_format': 2,
    }

    filename = pathlib.Path('/vsimem/too_many_samples_3d.zarr')
    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    open_options = ['MULTIBAND=NO']
    with gdal.quiet_errors():
      with gdal.OpenEx(filename, open_options=open_options) as ds:
        message = 'Too many samples along the > 2D dimensions'
        self.assertIn(message, gdal.GetLastErrorMsg())
        self.assertEmpty(ds.GetSubDatasets())

    gdal.ErrorReset()

    open_options = ['MULTIBAND=YES']

    with gdal.quiet_errors():
      message = 'Too many bands. Operate on a sliced view'
      with self.assertRaisesRegex(RuntimeError, message):
        gdal.OpenEx(filename, open_options=open_options)

      with no_gdal_exceptions():
        self.assertIsNone(gdal.OpenEx(filename, open_options=open_options))

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(['BAND', 'PIXEL'])
  def test_zarr_write_single_array_3d(self, interleave: str):
    src_filename = gdrivers_util.GetTestFilePath('gtiff/rgbsmall.tif')
    src_ds = gdal.Open(src_filename)

    filename = pathlib.Path(f'/vsimem/write_single_array_3d_{interleave}.zarr')

    options = [f'INTERLEAVE={interleave}']

    with self.driver.CreateCopy(filename, src_ds, options=options) as ds:
      result = [
          ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)
      ]
      expect = [
          src_ds.GetRasterBand(i + 1).Checksum()
          for i in range(src_ds.RasterCount)
      ]
      self.assertEqual(result, expect)

      result = [
          ds.GetRasterBand(i + 1).GetColorInterpretation()
          for i in range(ds.RasterCount)
      ]
      expect = [gdal.GCI_RedBand, gdal.GCI_GreenBand, gdal.GCI_BlueBand]
      self.assertEqual(result, expect)

    gdal.RmdirRecursive(filename)

  def test_zarr_read_classic_4d(self):
    j = {
        'chunks': [3, 2, 1, 1],
        'compressor': None,
        'dtype': '!u1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [3, 2, 1, 1],
        'zarr_format': 2,
    }

    filename = pathlib.Path('/vsimem/test_zarr_read_classic_4d.zarr')
    gdal.Mkdir(filename, 755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    with gdal.OpenEx(filename, open_options=['MULTIBAND=NO']) as ds:
      subds = ds.GetSubDatasets()
      self.assertLen(subds, 6)
      for sub_dataset in subds:
        self.assertIsNotNone(gdal.Open(sub_dataset[0]))

    gdal.RmdirRecursive(filename)

  def test_zarr_read_classic_too_many_samples_4d(self):
    j = {
        'chunks': [257, 256, 1, 1],
        'compressor': None,
        'dtype': '!u1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [257, 256, 1, 1],
        'zarr_format': 2,
    }

    filename = pathlib.Path('/vsimem/too_many_samples_4d.zarr')

    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    open_options = ['MULTIBAND=NO']
    with no_gdal_exceptions():
      with gdal.quiet_errors():
        with gdal.OpenEx(filename, open_options=open_options) as ds:
          message = 'Too many samples along the > 2D dimensions of'
          self.assertIn(message, gdal.GetLastErrorMsg())
          self.assertEmpty(ds.GetSubDatasets())

    gdal.RmdirRecursive(filename)

  def test_zarr_read_empty_shape(self):
    filename = self.getTestFilePath('empty.zarr')
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('b', [120]))

  # Skip test_zarr_read_BLOSC_COMPRESSORS.
  # Covered by test_zarr_compressor_availability.

  # ZARR_V3 does not create a .zmetadata file when CREATE_ZMETADATA is 'YES'.
  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_create_group(self, zarr_format: str, create_zmetadata: str):
    filename = pathlib.Path(
        f'/vsimem/create_group_{format}_{create_zmetadata}.zarr'
    )

    def create():
      # The create must happen in a function to get *all* the files to flush.
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_zmetadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      self.assertEqual(rg.GetName(), '/')

      attr = rg.CreateAttribute(
          'str_attr', [], gdal.ExtendedDataType.CreateString()
      )
      self.assertEqual(attr.GetFullName(), '/_GLOBAL_/str_attr')
      self.assertEqual(attr.Write('my_string'), gdal.CE_None)

      attr = rg.CreateAttribute(
          'json_attr',
          [],
          gdal.ExtendedDataType.CreateString(0, gdal.GEDTST_JSON),
      )
      self.assertEqual(attr.Write({'foo': 'bar'}), gdal.CE_None)

      attr = rg.CreateAttribute(
          'str_array_attr', [2], gdal.ExtendedDataType.CreateString()
      )
      self.assertEqual(
          attr.Write(['first_string', 'second_string']), gdal.CE_None
      )

      message = 'Cannot create attributes of dimension >= 2'
      with self.assertRaisesRegex(RuntimeError, message):
        rg.CreateAttribute(
            'dim_2_not_supported', [2, 2], gdal.ExtendedDataType.CreateString()
        )

      attr = rg.CreateAttribute(
          'int_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Int32)
      )
      self.assertEqual(attr.Write(12345678), gdal.CE_None)

      attr = rg.CreateAttribute(
          'uint_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_UInt32)
      )
      self.assertEqual(attr.Write(4000000000), gdal.CE_None)

      attr = rg.CreateAttribute(
          'int64_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Int64)
      )
      self.assertEqual(attr.Write(12345678901234), gdal.CE_None)

      attr = rg.CreateAttribute(
          'uint64_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_UInt64)
      )
      # Cannot write UINT64_MAX.
      # self.assertEqual(attr.Write(18000000000000000000)
      self.assertEqual(attr.Write(9000000000000000000), gdal.CE_None)

      attr = rg.CreateAttribute(
          'int_array_attr', [2], gdal.ExtendedDataType.Create(gdal.GDT_Int32)
      )
      self.assertEqual(attr.Write([12345678, -12345678]), gdal.CE_None)

      attr = rg.CreateAttribute(
          'uint_array_attr', [2], gdal.ExtendedDataType.Create(gdal.GDT_UInt32)
      )
      self.assertEqual(attr.Write([12345678, 4000000000]), gdal.CE_None)

      attr = rg.CreateAttribute(
          'int64_array_attr', [2], gdal.ExtendedDataType.Create(gdal.GDT_Int64)
      )
      self.assertEqual(
          attr.Write([12345678091234, -12345678091234]), gdal.CE_None
      )

      attr = rg.CreateAttribute(
          'uint64_array_attr',
          [2],
          gdal.ExtendedDataType.Create(gdal.GDT_UInt64),
      )
      # Cannot write UINT64_MAX.
      # attr.Write([12345678091234, 18000000000000000000])
      self.assertEqual(
          attr.Write([12345678091234, 9000000000000000000]), gdal.CE_None
      )

      attr = rg.CreateAttribute(
          'double_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Float64)
      )
      self.assertEqual(attr.Write(12345678.5), gdal.CE_None)

      attr = rg.CreateAttribute(
          'double_array_attr',
          [2],
          gdal.ExtendedDataType.Create(gdal.GDT_Float64),
      )
      self.assertEqual(attr.Write([12345678.5, -12345678.5]), gdal.CE_None)

      subgroup = rg.CreateGroup('foo')
      self.assertEqual(subgroup.GetName(), 'foo')
      self.assertEqual(subgroup.GetFullName(), '/foo')
      self.assertEqual(rg.GetGroupNames(), ['foo'])
      self.assertIsNotNone(rg.OpenGroup('foo'))

    create()

    if create_zmetadata == 'YES':
      metadata_path = filename / '.zmetadata'
      f = gdal.VSIFOpenL(str(metadata_path), 'r')
      data = gdal.VSIFReadL(1, 10000, f)
      gdal.VSIFCloseL(f)
      j = json.loads(data)
      self.assertIn('foo/.zgroup', j['metadata'])

    def update():
      # The update must happen in a function to get *all* the files to flush.
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()

      attr = rg.GetAttribute('str_attr')
      self.assertEqual(attr.Read(), 'my_string')
      self.assertEqual(attr.Write('my_string_modified'), gdal.CE_None)

      self.assertIsNotNone(rg.OpenGroup('foo'))
      self.assertIsNotNone(rg.CreateGroup('bar'))
      self.assertEqual(set(rg.GetGroupNames()), set(['foo', 'bar']))
      subgroup = rg.OpenGroup('foo')
      self.assertIsNotNone(subgroup.CreateGroup('baz'))

    update()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()

      attr = rg.GetAttribute('str_attr')
      self.assertEqual(attr.Read(), 'my_string_modified')

      attr = rg.GetAttribute('json_attr')
      self.assertEqual(attr.GetDataType().GetSubType(), gdal.GEDTST_JSON)
      self.assertEqual(attr.Read(), {'foo': 'bar'})

      attr = rg.GetAttribute('str_array_attr')
      self.assertEqual(attr.Read(), ['first_string', 'second_string'])

      attr = rg.GetAttribute('int_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int32)
      self.assertEqual(attr.ReadAsInt(), 12345678)
      self.assertEqual(attr.ReadAsInt64(), 12345678)
      self.assertEqual(attr.ReadAsDouble(), 12345678)

      attr = rg.GetAttribute('uint_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int64)
      self.assertEqual(attr.ReadAsInt64(), 4000000000)
      self.assertEqual(attr.ReadAsDouble(), 4000000000)

      attr = rg.GetAttribute('int64_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int64)
      self.assertEqual(attr.ReadAsInt64(), 12345678901234)
      self.assertEqual(attr.ReadAsDouble(), 12345678901234)

      attr = rg.GetAttribute('uint64_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int64)
      self.assertEqual(attr.ReadAsInt64(), 9000000000000000000)
      self.assertEqual(attr.ReadAsDouble(), 9000000000000000000)

      attr = rg.GetAttribute('int_array_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int32)
      self.assertEqual(attr.ReadAsIntArray(), (12345678, -12345678))
      self.assertEqual(attr.ReadAsInt64Array(), (12345678, -12345678))
      self.assertEqual(attr.ReadAsDoubleArray(), (12345678, -12345678))

      attr = rg.GetAttribute('uint_array_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int64)
      self.assertEqual(attr.ReadAsInt64Array(), (12345678, 4000000000))
      self.assertEqual(attr.ReadAsDoubleArray(), (12345678, 4000000000))

      attr = rg.GetAttribute('int64_array_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int64)
      self.assertEqual(
          attr.ReadAsInt64Array(), (12345678091234, -12345678091234)
      )
      self.assertEqual(
          attr.ReadAsDoubleArray(), (12345678091234, -12345678091234)
      )

      attr = rg.GetAttribute('uint64_array_attr')
      self.assertEqual(attr.GetDataType().GetNumericDataType(), gdal.GDT_Int64)
      self.assertEqual(
          attr.ReadAsInt64Array(), (12345678091234, 9000000000000000000)
      )
      self.assertEqual(
          attr.ReadAsDoubleArray(), (12345678091234, 9000000000000000000)
      )

      attr = rg.GetAttribute('double_attr')
      self.assertEqual(
          attr.GetDataType().GetNumericDataType(), gdal.GDT_Float64
      )
      self.assertEqual(attr.ReadAsDouble(), 12345678.5)

      attr = rg.GetAttribute('double_array_attr')
      self.assertEqual(
          attr.GetDataType().GetNumericDataType(), gdal.GDT_Float64
      )
      self.assertEqual(attr.Read(), (12345678.5, -12345678.5))

      self.assertEqual(set(rg.GetGroupNames()), set(['foo', 'bar']))

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        rg.CreateGroup('not_opened_in_update_mode')

      subgroup = rg.OpenGroup('foo')
      self.assertIsNotNone(subgroup.OpenGroup('baz'))

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      'foo',  # already existing
      'directory_with_that_name',
      '',
      '.',
      '..',
      'a/b',
      'a\\n',
      'a:b',
      '.zarray',
  ])
  def test_zarr_create_group_errors(self, group_name: str):
    for zarr_format in ['ZARR_V2', 'ZARR_V3']:
      options = [f'FORMAT={zarr_format}']
      filename = pathlib.Path(
          f'/vsimem/create_group_errors_{zarr_format}_{group_name}.zarr'
      )

      with self.driver.CreateMultiDimensional(filename, options=options) as ds:
        rg = ds.GetRootGroup()
        self.assertIsNotNone(rg.CreateGroup('foo'))
        gdal.Mkdir(filename / 'directory_with_that_name', 0o755)
        with self.assertRaises(RuntimeError):
          rg.CreateGroup(group_name)

      gdal.RmdirRecursive(filename)

  # TODO: b/389359730 - This is overly complicated. Consider splitting it up.
  @parameterized.parameters([
      (gdal.ExtendedDataType.Create(gdal.GDT_Byte), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Byte), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_UInt16), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_UInt16), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Int16), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_UInt32), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Int32), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float16), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float32), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), 1.5, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), math.nan, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), math.inf, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), -math.inf, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_CInt16), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_CInt32), None, 'ZARR_V2'),
      # CFloat16 is not yet supported in Python
      # (gdal.ExtendedDataType.Create(gdal.GDT_CFloat16), None),
      (gdal.ExtendedDataType.Create(gdal.GDT_CFloat32), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.Create(gdal.GDT_CFloat64), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.CreateString(10), None, 'ZARR_V2'),
      (gdal.ExtendedDataType.CreateString(10), 'ab', 'ZARR_V2'),

      (gdal.ExtendedDataType.Create(gdal.GDT_Byte), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Byte), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_UInt16), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_UInt16), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Int16), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_UInt32), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Int32), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float16), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float32), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), 1.5, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), math.nan, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), math.inf, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_Float64), -math.inf, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_CInt16), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_CInt32), None, 'ZARR_V3'),
      # CFloat16 is not yet supported in Python
      # (gdal.ExtendedDataType.Create(gdal.GDT_CFloat16), None),
      (gdal.ExtendedDataType.Create(gdal.GDT_CFloat32), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.Create(gdal.GDT_CFloat64), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.CreateString(10), None, 'ZARR_V3'),
      (gdal.ExtendedDataType.CreateString(10), 'ab', 'ZARR_V3'),
  ])
  def test_zarr_create_array(self, datatype, nodata, zarr_format):
    error_expected = False
    if datatype.GetNumericDataType() in (gdal.GDT_CInt16, gdal.GDT_CInt32):
      error_expected = True
    elif zarr_format == 'ZARR_V3' and datatype.GetClass() != gdal.GEDTC_NUMERIC:
      error_expected = True

    filename = pathlib.Path(
        f'/vsimem/create_array_{datatype}_{nodata}_{zarr_format}.zarr'
    )

    def create() -> bool:
      # The create must happen in a function to get *all* the files to flush.
      options = [
          f'FORMAT={zarr_format}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      self.assertEqual(rg.GetName(), '/')

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim1 = rg.CreateDimension('dim1', None, None, 3)

      if error_expected:
        # The text that follows this message is not consistent across cases.
        message = 'Unsupported data type'
        with self.assertRaisesRegex(RuntimeError, message):
          rg.CreateMDArray('my_ar', [dim0, dim1], datatype)
        return False
      ar = rg.CreateMDArray('my_ar', [dim0, dim1], datatype)
      if nodata:
        if datatype.GetClass() == gdal.GEDTC_STRING:
          self.assertEqual(ar.SetNoDataValueString(nodata), gdal.CE_None)
        elif datatype.GetClass() == gdal.GEDTC_NUMERIC:
          self.assertEqual(ar.SetNoDataValueDouble(nodata), gdal.CE_None)
        else:
          self.assertEqual(ar.SetNoDataValueRaw(nodata), gdal.CE_None)
      return True

    if create():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('my_ar')
      got_dt = ar.GetDataType()
      if got_dt.GetClass() == gdal.GEDTC_COMPOUND:
        comps = got_dt.GetComponents()
        self.assertLen(comps, 2)
        self.assertEqual(comps[1].GetType().GetClass(), gdal.GEDTC_COMPOUND)
        comps[1] = gdal.EDTComponent.Create(
            comps[1].GetName(),
            comps[1].GetType().GetSize(),
            gdal.ExtendedDataType.CreateCompound(
                '',
                comps[1].GetType().GetSize(),
                comps[1].GetType().GetComponents(),
            ),
        )
        got_dt = gdal.ExtendedDataType.CreateCompound(
            '', got_dt.GetSize(), comps
        )
        self.assertEqual(got_dt, datatype)
        self.assertLen(ar.GetDimensions(), 2)
        self.assertEqual(
            [ar.GetDimensions()[i].GetSize() for i in range(2)], [2, 3]
        )
        if nodata:
          if datatype.GetClass() == gdal.GEDTC_STRING:
            got_nodata = ar.GetNoDataValueAsString()
            self.assertEqual(got_nodata, nodata)
          elif datatype.GetClass() == gdal.GEDTC_NUMERIC:
            got_nodata = ar.GetNoDataValueAsDouble()
            if math.isnan(nodata):
              self.assertTrue(math.isnan(got_nodata))
            else:
              self.assertEqual(got_nodata, nodata)
          else:
            got_nodata = ar.GetNoDataValueAsRaw()
            self.assertEqual(got_nodata, nodata)
        else:
          if zarr_format == 'ZARR_V3':
            self.assertTrue(
                ar.GetNoDataValueAsRaw() is None
                or math.isnan(ar.GetNoDataValueAsDouble())
            )
          else:
            self.assertIsNone(ar.GetNoDataValueAsRaw())

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      (
          'foo',
          'An array with same name already exists',
          'ZARR_V2',
      ),
      (
          'directory_with_that_name',
          'directory_with_that_name already exists',
          'ZARR_V2',
      ),
      ('', 'Invalid array name', 'ZARR_V2'),
      ('.', 'Invalid array name', 'ZARR_V2'),
      ('..', 'Invalid array name', 'ZARR_V2'),
      ('a/b', 'Invalid array name', 'ZARR_V2'),
      ('a\\n', 'Invalid array name', 'ZARR_V2'),
      ('a:b', 'Invalid array name', 'ZARR_V2'),
      ('.zarray', 'Invalid array name', 'ZARR_V2'),
      (
          'foo',
          'An array with same name already exists',
          'ZARR_V3',
      ),
      (
          'directory_with_that_name',
          'directory_with_that_name already exists',
          'ZARR_V3',
      ),
      ('', 'Invalid array name', 'ZARR_V3'),
      ('.', 'Invalid array name', 'ZARR_V3'),
      ('..', 'Invalid array name', 'ZARR_V3'),
      ('a/b', 'Invalid array name', 'ZARR_V3'),
      ('a\\n', 'Invalid array name', 'ZARR_V3'),
      ('a:b', 'Invalid array name', 'ZARR_V3'),
      ('.zarray', 'Invalid array name', 'ZARR_V3'),
  ])
  def test_zarr_create_array_errors(
      self, array_name: str, message: str, zarr_format: str
  ):
    filename = pathlib.Path(f'/vsimem/create_array_errors_{zarr_format}.zarr')

    def at_creation():
      options = [f'FORMAT={zarr_format}']
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      self.assertIsNotNone(
          rg.CreateMDArray(
              'foo', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
          )
      )
      gdal.Mkdir(filename / 'directory_with_that_name', 0o755)

      with self.assertRaisesRegex(RuntimeError, message):
        rg.CreateMDArray(
            array_name, [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )

    at_creation()

    open_options = gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE
    with gdal.OpenEx(filename, open_options) as ds:
      rg = ds.GetRootGroup()

      with self.assertRaisesRegex(RuntimeError, message):
        rg.CreateMDArray(
            array_name, [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ('NONE', [], None),
      ('zlib', [], {'id': 'zlib', 'level': 6}),
      ('zlib', ['ZLIB_LEVEL=1'], {'id': 'zlib', 'level': 1}),
      (
          'blosc',
          [],
          {
              'blocksize': 0,
              'clevel': 5,
              'cname': 'lz4',
              'id': 'blosc',
              'shuffle': 1,
          },
      ),
  ])
  def test_zarr_create_array_compressor(
      self,
      compressor,
      compressor_options,
      expected_json,
  ):
    filename = pathlib.Path(
        f'/vsimem/create_array_compressor_{compressor}.zarr'
    )

    def create():
      ds = self.driver.CreateMultiDimensional(filename)
      rg = ds.GetRootGroup()
      options = [f'COMPRESS={compressor}'] + compressor_options
      self.assertIsNotNone(
          rg.CreateMDArray(
              'test', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte), options
          )
      )

    create()

    zarray_filename = filename / 'test/.zarray'

    f = gdal.VSIFOpenL(zarray_filename, 'rb')
    data = gdal.VSIFReadL(1, 1000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    self.assertEqual(j['compressor'], expected_json)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ('NONE', [], [{'name': 'bytes', 'configuration': {'endian': 'little'}}]),
      (
          'gzip',
          [],
          [
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {'name': 'gzip', 'configuration': {'level': 6}},
          ],
      ),
      (
          'gzip',
          ['GZIP_LEVEL=1'],
          [
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {'name': 'gzip', 'configuration': {'level': 1}},
          ],
      ),
      (
          'blosc',
          [],
          [
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {
                  'name': 'blosc',
                  'configuration': {
                      'cname': 'lz4',
                      'clevel': 5,
                      'shuffle': 'shuffle',
                      'typesize': 1,
                      'blocksize': 0,
                  },
              },
          ],
      ),
      (
          'blosc',
          [
              'BLOSC_CNAME=zlib',
              'BLOSC_CLEVEL=1',
              'BLOSC_SHUFFLE=NONE',
              'BLOSC_BLOCKSIZE=2',
          ],
          [
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {
                  'name': 'blosc',
                  'configuration': {
                      'cname': 'zlib',
                      'clevel': 1,
                      'shuffle': 'noshuffle',
                      'blocksize': 2,
                  },
              },
          ],
      ),
      (
          'zstd',
          ['ZSTD_LEVEL=20'],
          [
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {
                  'name': 'zstd',
                  'configuration': {'level': 20, 'checksum': False},
              },
          ],
      ),
      (
          'zstd',
          ['ZSTD_CHECKSUM=YES'],
          [
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {
                  'name': 'zstd',
                  'configuration': {'level': 13, 'checksum': True},
              },
          ],
      ),
  ])
  def test_zarr_create_array_compressor_v3(
      self,
      compressor,
      compressor_options,
      expected_json,
  ):
    filename = pathlib.Path(
        f'/vsimem/create_array_compressor_v3_{compressor}.zarr'
    )

    def create():
      options = ['FORMAT=ZARR_V3']
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      dim = rg.CreateDimension('dim0', None, None, 2)
      ar = rg.CreateMDArray(
          'test',
          [dim],
          gdal.ExtendedDataType.Create(gdal.GDT_Byte),
          [f'COMPRESS={compressor}'] + compressor_options,
      )
      self.assertEqual(ar.Write(array.array('b', [1, 2])), gdal.CE_None)

    create()

    f = gdal.VSIFOpenL(filename / 'test/zarr.json', 'rb')
    data = gdal.VSIFReadL(1, 1000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    if expected_json is None:
      self.assertNotIn('codecs', j)
    else:
      self.assertEqual(j['codecs'], expected_json)

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('test')
      self.assertEqual(ar.Read(), array.array('b', [1, 2]))

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      (
          ['@ENDIAN=little'],
          [{'configuration': {'endian': 'little'}, 'name': 'bytes'}],
      ),
      (
          ['@ENDIAN=big'],
          [{'configuration': {'endian': 'big'}, 'name': 'bytes'}],
      ),
      (
          ['@ENDIAN=little', 'CHUNK_MEMORY_LAYOUT=F'],
          [
              {'name': 'transpose', 'configuration': {'order': 'F'}},
              {'configuration': {'endian': 'little'}, 'name': 'bytes'},
          ],
      ),
      (
          ['@ENDIAN=big', 'CHUNK_MEMORY_LAYOUT=F'],
          [
              {'name': 'transpose', 'configuration': {'order': 'F'}},
              {'configuration': {'endian': 'big'}, 'name': 'bytes'},
          ],
      ),
      (
          ['@ENDIAN=big', 'CHUNK_MEMORY_LAYOUT=F', 'COMPRESS=GZIP'],
          [
              {'name': 'transpose', 'configuration': {'order': 'F'}},
              {'name': 'bytes', 'configuration': {'endian': 'big'}},
              {'name': 'gzip', 'configuration': {'level': 6}},
          ],
      ),
  ])
  def test_zarr_create_array_endian_v3(self, array_options, expected_json):
    gdal_data_types = [
        gdal.GDT_Int8,
        gdal.GDT_Byte,
        gdal.GDT_Int16,
        gdal.GDT_UInt16,
        gdal.GDT_Int32,
        gdal.GDT_UInt32,
        gdal.GDT_Int64,
        gdal.GDT_UInt64,
        # Float16 not yet supported by SWIG
        # gdal.GDT_Float16,
        gdal.GDT_Float32,
        gdal.GDT_Float64,
    ]

    for gdal_data_type in gdal_data_types:

      def create(array_type, filename):
        options = ['FORMAT=ZARR_V3']
        ds = self.driver.CreateMultiDimensional(filename, options=options)
        rg = ds.GetRootGroup()
        dim0 = rg.CreateDimension('dim0', None, None, 2)
        ar = rg.CreateMDArray(
            'test',
            [dim0],
            gdal.ExtendedDataType.Create(gdal_data_type),
            array_options,
        )
        self.assertEqual(
            ar.Write(array.array(array_type, [1, 2])), gdal.CE_None
        )

      filename = pathlib.Path(
          f'/vsimem/create_array_endian_v3_{gdal_data_type}.zarr'
      )
      array_type = _gdal_data_type_to_array_type[gdal_data_type]

      create(array_type, filename)

      f = gdal.VSIFOpenL(filename / 'test/zarr.json', 'rb')
      data = gdal.VSIFReadL(1, 1000, f)
      gdal.VSIFCloseL(f)
      j = json.loads(data)
      self.assertEqual(j['codecs'], expected_json)

      with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
        rg = ds.GetRootGroup()
        ar = rg.OpenMDArray('test')
        self.assertEqual(ar.Read(), array.array(array_type, [1, 2]))

      gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'MISSING_shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'shape missing or not an array',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': 'invalid',
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'shape missing or not an array',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1, 2]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'shape and chunks arrays are of different size',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'MISSING_data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'data_type missing',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8_INVALID',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'Invalid or unsupported format for data_type',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'MISSING_chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'chunk_grid missing or not an object',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {'name': 'invalid'},
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'Only chunk_grid.name = regular supported',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {'name': 'regular'},
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'chunk_grid.configuration.chunk_shape missing or not an array',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'MISSING_chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'chunk_key_encoding missing or not an object',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'invalid'},
              'fill_value': 0,
          },
          'Unsupported chunk_key_encoding.name',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {
                  'name': 'default',
                  'configuration': {'separator': 'invalid'},
              },
              'fill_value': 0,
          },
          r"Separator can only be '/' or '\.'",
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
              'storage_transformers': [{}],
          },
          'storage_transformers are not supported',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 'invalid',
          },
          'Invalid fill_value',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': '0',
              'dimension_names': 'invalid',
          },
          'dimension_names should be an array',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': '0',
              'dimension_names': [],
          },
          r'Size of dimension_names\[] different from the one of shape',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 'NaN',
          },
          'Invalid fill_value for this data type',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': '0x00',
          },
          (
              'Hexadecimal representation of fill_value no supported for this'
              ' data type'
          ),
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': '0b00',
          },
          'Binary representation of fill_value no supported for this data type',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1 << 40, 1 << 40],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1 << 40, 1 << 40]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          'Too large chunks',
      ),
      (
          {
              'zarr_format': 3,
              'node_type': 'array',
              'shape': [1 << 30, 1 << 30, 1 << 30],
              'data_type': 'uint8',
              'chunk_grid': {
                  'name': 'regular',
                  'configuration': {'chunk_shape': [1, 1, 1]},
              },
              'chunk_key_encoding': {'name': 'default'},
              'fill_value': 0,
          },
          r'has more than 2\^64 tiles',
      ),
  ])
  def test_zarr_read_invalid_zarr_v3(self, j, error_message):
    filename = pathlib.Path('/vsimem/read_invalid_zarr_v3.zarr')
    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / 'zarr.json', json.dumps(j))

    # Getting warnings such as:
    #   array definition contains a unknown member (MISSING_shape).
    #   Interpretation of the array might be wrong.
    with gdal.quiet_warnings():
      with self.assertRaisesRegex(RuntimeError, error_message):
        gdal.Open(filename)

    gdal.RmdirRecursive(filename)

  def test_zarr_read_data_type_fallback_zarr_v3(self):
    j = {
        'zarr_format': 3,
        'node_type': 'array',
        'shape': [1],
        'data_type': {
            'name': 'datetime',
            'configuration': {'unit': 'ns'},
            'fallback': 'int64',
        },
        'chunk_grid': {
            'name': 'regular',
            'configuration': {'chunk_shape': [1]},
        },
        'chunk_key_encoding': {'name': 'default'},
        'fill_value': 0,
    }

    filename = pathlib.Path(
        '/vsimem/test_zarr_read_data_type_fallback_zarr_v3.zarr'
    )

    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / 'zarr.json', json.dumps(j))
    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Int64)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      # JSON NoDataValues cannot be Float16
      # ("float16", "0x3e00", 1.5),
      # ("float16", str(bin(0x3E00)), 1.5),
      ('float32', '0x3fc00000', 1.5),
      ('float32', str(bin(0x3FC00000)), 1.5),
      ('float64', '0x3ff8000000000000', 1.5),
      ('float64', str(bin(0x3FF8000000000000)), 1.5),
  ])
  def test_zarr_read_fill_value_v3(self, data_type, fill_value, nodata):
    j = {
        'zarr_format': 3,
        'node_type': 'array',
        'shape': [1],
        'data_type': data_type,
        'chunk_grid': {
            'name': 'regular',
            'configuration': {'chunk_shape': [1]},
        },
        'chunk_key_encoding': {'name': 'default'},
        'fill_value': fill_value,
    }

    filename = pathlib.Path('/vsimem/test_zarr_read_fill_value_v3.zarr')
    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / 'zarr.json', json.dumps(j))
    with gdal.Open(filename) as ds:
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), nodata)

    gdal.RmdirRecursive(filename)

  # complex32 is not yet supported by Python.
  @parameterized.parameters([
      ('complex128', [1, 2], [1, 2]),
      ('complex128', [1.5, 'NaN'], [1.5, math.nan]),
      ('complex128', [1234567890123, 'Infinity'], [1234567890123, math.inf]),
      ('complex128', [1, '-Infinity'], [1, -math.inf]),
      ('complex128', ['NaN', 2.5], [math.nan, 2.5]),
      ('complex128', ['Infinity', 2], [math.inf, 2]),
      ('complex128', ['-Infinity', 2], [-math.inf, 2]),
      ('complex128', ['0x7ff8000000000000', 2], [math.nan, 2]),
      # Invalid ones
      ('complex128', 1, None),
      ('complex128', 'NaN', None),
      ('complex128', [], None),
      ('complex128', [1, 2, 3], None),
      ('complex128', ['invalid', 1], None),
      ('complex128', [1, 'invalid'], None),
      # complex64.
      ('complex64', [1, 2], [1, 2]),
      ('complex64', [1.5, 'NaN'], [1.5, math.nan]),
      ('complex64', [1234567890123, 'Infinity'], [1234567890123, math.inf]),
      ('complex64', [1, '-Infinity'], [1, -math.inf]),
      ('complex64', ['NaN', 2.5], [math.nan, 2.5]),
      ('complex64', ['Infinity', 2], [math.inf, 2]),
      ('complex64', ['-Infinity', 2], [-math.inf, 2]),
      ('complex64', ['0x7ff8000000000000', 2], [math.nan, 2]),
      # Invalid ones
      ('complex64', 1, None),
      ('complex64', 'NaN', None),
      ('complex64', [], None),
      ('complex64', [1, 2, 3], None),
      ('complex64', ['invalid', 1], None),
      ('complex64', [1, 'invalid'], None),
  ])
  def test_zarr_read_fill_value_complex_datatype_v3(
      self, data_type, fill_value, nodata
  ):
    if fill_value and isinstance(fill_value, list):
      # float32 precision not sufficient to hold 1234567890123.
      if data_type == 'complex64' and fill_value[0] == 1234567890123:
        fill_value[0] = 123456
        nodata[0] = 123456
      # float16 precision not sufficient to hold 1234567890123.
      if data_type == 'complex32' and fill_value[0] == 1234567890123:
        fill_value[0] = 1234
        nodata[0] = 1234

      # Convert float64 nan hexadecimal representation to float32.
      if (
          data_type == 'complex64'
          and str(fill_value[0]) == '0x7ff8000000000000'
      ):
        fill_value[0] = '0x7fc00000'
      # Convert float64 nan hexadecimal representation to float16.
      if (
          data_type == 'complex32'
          and str(fill_value[0]) == '0x7ff8000000000000'
      ):
        fill_value[0] = '0x7e00'

    filename = pathlib.Path('/vsimem/test.zarr')
    gdal.Mkdir(filename, 0o755)

    j = {
        'zarr_format': 3,
        'node_type': 'array',
        'shape': [1],
        'data_type': data_type,
        'chunk_grid': {
            'name': 'regular',
            'configuration': {'chunk_shape': [1]},
        },
        'chunk_key_encoding': {'name': 'default'},
        'fill_value': fill_value,
    }
    gdal.FileFromMemBuffer(filename / 'zarr.json', json.dumps(j))

    if nodata is None:
      with self.assertRaisesRegex(RuntimeError, 'Invalid fill_value') as ds:
        gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)

      gdal.RmdirRecursive(filename)
      return

    def open_and_modify():
      open_options = gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE
      ds = gdal.OpenEx(filename, open_options)
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('test')

      # TODO(schwehr): Correctly convert nodata to bytes.
      # dtype = (
      #     'd'
      #     if ar.GetDataType().GetNumericDataType() == gdal.GDT_CFloat64
      #     else 'f'
      # )
      # self.assertEqual(
      #     ar.GetNoDataValueAsRaw(), bytes(array.array(dtype, nodata))
      # )

      # Force a reserialization of the array.
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.CreateString()
      )
      attr.Write('foo')

    open_and_modify()

    if not str(fill_value[0]).startswith('0x'):
      f = gdal.VSIFOpenL(filename / 'zarr.json', 'rb')
      data = gdal.VSIFReadL(1, 10000, f)
      gdal.VSIFCloseL(f)
      j = json.loads(data)
      self.assertEqual(j['fill_value'], fill_value)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_create_array_bad_compressor(self, zarr_format: str):
    filename = pathlib.Path(f'/vsimem/array_bad_compressor_{zarr_format}.zarr')
    options = ['FORMAT=' + zarr_format]
    with self.driver.CreateMultiDimensional(filename, options=options) as ds:
      rg = ds.GetRootGroup()
      with self.assertRaisesRegex(RuntimeError, 'invalid'):
        data_type = gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        rg.CreateMDArray('test', [], data_type, ['COMPRESS=invalid'])

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_create_array_attributes(self, zarr_format: str):
    filename = pathlib.Path(f'/vsimem/array_attributes_{zarr_format}.zarr')

    def create():
      options = ['FORMAT=' + zarr_format]
      with self.driver.CreateMultiDimensional(filename, options=options) as ds:
        rg = ds.GetRootGroup()
        ar = rg.CreateMDArray(
            'test', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )
        self.assertEqual(ar.GetFullName(), '/test')

        attr = ar.CreateAttribute(
            'str_attr', [], gdal.ExtendedDataType.CreateString()
        )
        self.assertEqual(attr.GetFullName(), '/test/str_attr')
        self.assertEqual(attr.Write('my_string'), gdal.CE_None)

        message = 'Cannot create attributes of dimension >= 2'
        with self.assertRaisesRegex(RuntimeError, message):
          ar.CreateAttribute(
              'invalid_2d', [2, 3], gdal.ExtendedDataType.CreateString()
          )

    create()

    def update():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('test')

      attr = ar.GetAttribute('str_attr')
      self.assertEqual(attr.Read(), 'my_string')
      self.assertEqual(attr.Write('my_string_modified'), gdal.CE_None)

    update()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('test')

      attr = ar.GetAttribute('str_attr')
      self.assertEqual(attr.Read(), 'my_string_modified')
      with self.assertRaisesRegex(RuntimeError, 'Non updatable object'):
        attr.Write('foo')

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        ar.CreateAttribute(
            'another_attr', [], gdal.ExtendedDataType.CreateString()
        )

    gdal.RmdirRecursive(filename)

  def test_zarr_create_array_set_crs(self):
    filename = pathlib.Path('/vsimem/array_set_crs.zarr')

    def create():
      ds = self.driver.CreateMultiDimensional(filename)
      rg = ds.GetRootGroup()
      ar = rg.CreateMDArray(
          'test', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      crs = osr.SpatialReference()
      crs.ImportFromEPSG(4326)
      self.assertEqual(ar.SetSpatialRef(crs), gdal.CE_None)

    create()

    f = gdal.VSIFOpenL(filename / 'test/.zattrs', 'rb')
    data = gdal.VSIFReadL(1, 10000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    self.assertIn('_CRS', j)
    crs = j['_CRS']
    self.assertIn('wkt', crs)
    self.assertIn('url', crs)
    self.assertEqual(crs['projjson']['type'], 'GeographicCRS')

    gdal.RmdirRecursive(filename)

  def test_zarr_create_array_set_dimension_name(self):
    filename = pathlib.Path('/vsimem/array_set_dimension_name.zarr')

    def create():
      ds = self.driver.CreateMultiDimensional(filename)
      rg = ds.GetRootGroup()
      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim0_ar = rg.CreateMDArray(
          'dim0', [dim0], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      dim0.SetIndexingVariable(dim0_ar)

      rg.CreateMDArray(
          'test', [dim0], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )

    create()

    f = gdal.VSIFOpenL(filename / 'test/.zattrs', 'rb')
    data = gdal.VSIFReadL(1, 10000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    self.assertIn('_ARRAY_DIMENSIONS', j)
    self.assertEqual(j['_ARRAY_DIMENSIONS'], ['dim0'])

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ('!b1', gdal.GDT_Byte, None, None),
      ('!i1', gdal.GDT_Int16, None, None),
      ('!i1', gdal.GDT_Int16, -1, -1),
      ('!u1', gdal.GDT_Byte, None, None),
      ('!u1', gdal.GDT_Byte, '1', 1),
      ('<i2', gdal.GDT_Int16, None, None),
      ('>i2', gdal.GDT_Int16, None, None),
      ('<i4', gdal.GDT_Int32, None, None),
      ('>i4', gdal.GDT_Int32, None, None),
      ('<i8', gdal.GDT_Float64, None, None),
      ('>i8', gdal.GDT_Float64, None, None),
      ('<u2', gdal.GDT_UInt16, None, None),
      ('>u2', gdal.GDT_UInt16, None, None),
      ('<u4', gdal.GDT_UInt32, None, None),
      ('>u4', gdal.GDT_UInt32, None, None),
      ('<u4', gdal.GDT_UInt32, 4000000000, 4000000000),
      ('<u8', gdal.GDT_Float64, 4000000000, 4000000000),
      ('>u8', gdal.GDT_Float64, None, None),
      # TODO: b/389359730 - GDT_Float16 via float32 Python data
      # ("<f2", gdal.GDT_Float16, None, None),
      # (">f2", gdal.GDT_Float16, None, None),
      # ("<f2", gdal.GDT_Float16, 1.5, 1.5),
      # ("<f2", gdal.GDT_Float16, "NaN", float("nan")),
      # ("<f2", gdal.GDT_Float16, "Infinity", float("infinity")),
      # ("<f2", gdal.GDT_Float16, "-Infinity", float("-infinity")),
      ('<f4', gdal.GDT_Float32, None, None),
      ('>f4', gdal.GDT_Float32, None, None),
      ('<f4', gdal.GDT_Float32, 1.5, 1.5),
      ('<f4', gdal.GDT_Float32, 'NaN', math.nan),
      ('<f4', gdal.GDT_Float32, 'Infinity', math.inf),
      ('<f4', gdal.GDT_Float32, '-Infinity', -math.inf),
      ('<f8', gdal.GDT_Float64, None, None),
      ('>f8', gdal.GDT_Float64, None, None),
      ('<f8', gdal.GDT_Float64, 'NaN', math.nan),
      ('<f8', gdal.GDT_Float64, 'Infinity', math.inf),
      ('<f8', gdal.GDT_Float64, '-Infinity', -math.inf),
      # TODO: b/389359730 - GDT_CFloat16 via complex64 Python data
      # ("<c4", gdal.GDT_CFloat16, None, None),
      # (">c4", gdal.GDT_CFloat16, None, None),
      ('<c8', gdal.GDT_CFloat32, None, None),
      ('>c8', gdal.GDT_CFloat32, None, None),
      ('<c16', gdal.GDT_CFloat64, None, None),
      ('>c16', gdal.GDT_CFloat64, None, None),
  ])
  def test_zarr_write_array_content(
      self, dtype, gdaltype, fill_value, nodata_value
  ):
    structtype = _gdal_data_type_to_array_type[gdaltype]

    j = {
        'chunks': [2, 3],
        'compressor': None,
        'dtype': dtype,
        'fill_value': fill_value,
        'filters': None,
        'order': 'C',
        'shape': [5, 4],
        'zarr_format': 2,
    }

    for use_optimized_code_paths in (False, True):
      filename = pathlib.Path(
          '/vsimem/test_zarr_write_array_content_'
          + dtype.replace('<', 'lt').replace('>', 'gt').replace('!', 'not')
          + structtype
          + str(use_optimized_code_paths)
          + '.zarr'
      )

      gdal.Mkdir(filename, 0o755)
      f = gdal.VSIFOpenL(filename / '.zarray', 'wb')
      data = json.dumps(j)
      gdal.VSIFWriteL(data, 1, len(data), f)
      gdal.VSIFCloseL(f)

      complex_types = (gdal.GDT_CFloat16, gdal.GDT_CFloat32, gdal.GDT_CFloat64)

      if gdaltype not in complex_types:
        tile_0_0_data = struct.pack(
            dtype[0] + (structtype * 6), 1, 2, 3, 5, 6, 7
        )
        tile_0_1_data = struct.pack(
            dtype[0] + (structtype * 6), 4, 0, 0, 8, 0, 0
        )
      else:
        tile_0_0_data = struct.pack(
            dtype[0] + (structtype * 12), 1, 11, 2, 0, 3, 0, 5, 0, 6, 0, 7, 0
        )
        tile_0_1_data = struct.pack(
            dtype[0] + (structtype * 12), 4, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0
        )
      gdal.FileFromMemBuffer(filename / '0.0', tile_0_0_data)
      gdal.FileFromMemBuffer(filename / '0.1', tile_0_1_data)

      with gdal.config_option(
          'GDAL_ZARR_USE_OPTIMIZED_CODE_PATHS',
          'YES' if use_optimized_code_paths else 'NO',
      ):
        ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
        rg = ds.GetRootGroup()
        ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])

      self.assertIsNotNone(ar)

      dt = gdal.ExtendedDataType.Create(
          gdal.GDT_CFloat64
          if gdaltype in (gdal.GDT_CFloat32, gdal.GDT_CFloat64)
          else gdal.GDT_Float64
      )

      # Write all nodataset. That should cause tiles to be removed.
      nv = nodata_value if nodata_value else 0
      buf_nodata = array.array(
          'd',
          [nv] * (5 * 4 * (2 if gdaltype in complex_types else 1)),
      )
      self.assertEqual(ar.Write(buf_nodata, buffer_datatype=dt), gdal.CE_None)
      self.assertEqual(ar.Read(buffer_datatype=dt), bytearray(buf_nodata))

      if (
          fill_value is None
          or fill_value == 0
          or not gdal.DataTypeIsComplex(gdaltype)
      ):
        self.assertIsNone(gdal.VSIStatL(filename / '0.0'))

      # Write all ones
      ones = array.array(
          'd',
          [0] * (5 * 4 * (2 if gdaltype in complex_types else 1)),
      )
      self.assertEqual(ar.Write(ones, buffer_datatype=dt), gdal.CE_None)
      self.assertEqual(ar.Read(buffer_datatype=dt), bytearray(ones))

      # Write with odd array_step
      self.assertEqual(
          ar.Write(
              struct.pack('d' * 4, nv, nv, 6, 5),
              array_start_idx=[2, 1],
              count=[2, 2],
              array_step=[-1, -1],
              buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
          ),
          gdal.CE_None,
      )

      # Check back
      self.assertEqual(
          ar.Read(
              array_start_idx=[2, 1],
              count=[2, 2],
              array_step=[-1, -1],
              buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
          ),
          struct.pack('d' * 4, nv, nv, 6, 5),
      )

      # Force dirty block eviction
      ar.Read(buffer_datatype=dt)

      # Check back again
      self.assertEqual(
          ar.Read(
              array_start_idx=[2, 1],
              count=[2, 2],
              array_step=[-1, -1],
              buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Float64),
          ),
          struct.pack('d' * 4, nv, nv, 6, 5),
      )

      gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      (gdal.GDT_Byte, 'B'),
      (gdal.GDT_UInt16, 'H'),
      (gdal.GDT_UInt32, 'I'),
      (gdal.GDT_UInt64, 'Q'),
      (gdal.GDT_CFloat64, 'd'),
  ])
  def test_zarr_write_interleave(self, dt: int, array_type: str):
    filename = pathlib.Path(
        f'/vsimem/test_zarr_write_interleave_{array_type}.zarr'
    )

    def create():
      ds = self.driver.CreateMultiDimensional(filename)

      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 3)
      dim1 = rg.CreateDimension('dim1', None, None, 2)

      ar = rg.CreateMDArray(
          'test',
          [dim0, dim1],
          gdal.ExtendedDataType.Create(dt),
          ['BLOCKSIZE=2,2'],
      )
      self.assertEqual(
          ar.Write(
              array.array(
                  array_type,
                  [0, 2, 4]
                  if dt != gdal.GDT_CFloat64
                  else [0, 0.5, 2, 2.5, 4, 4.5],
              ),
              array_start_idx=[0, 0],
              count=[3, 1],
              array_step=[1, 0],
          ),
          gdal.CE_None,
      )
      self.assertEqual(
          ar.Write(
              array.array(
                  array_type,
                  [1, 3, 5]
                  if dt != gdal.GDT_CFloat64
                  else [1, 1.5, 3, 3.5, 5, 5.5],
              ),
              array_start_idx=[0, 1],
              count=[3, 1],
              array_step=[1, 0],
          ),
          gdal.CE_None,
      )

    create()

    open_options = gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE
    with gdal.OpenEx(filename, open_options) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(
          ar.Read(),
          array.array(
              array_type,
              [0, 1, 2, 3, 4, 5]
              if dt != gdal.GDT_CFloat64
              else [0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5],
          ),
      )
      if dt != gdal.GDT_CFloat64:
        self.assertEqual(
            ar.Read(
                buffer_datatype=gdal.ExtendedDataType.Create(gdal.GDT_Byte)
            ),
            array.array('B', [0, 1, 2, 3, 4, 5]),
        )

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ('ASCII', '0123456789truncated', '0123456789'),
      ('UNICODE', '\u00E9' + '123456789truncated', '\u00E9' + '123456789'),
  ])
  def test_zarr_create_array_string(
      self, string_format: str, input_str: str, output_str: str
  ):
    filename = pathlib.Path(f'/vsimem/create_array_string_{string_format}.zarr')

    with self.driver.CreateMultiDimensional(filename) as ds:
      rg = ds.GetRootGroup()
      dim0 = rg.CreateDimension('dim0', None, None, 2)

      ar = rg.CreateMDArray(
          'test',
          [dim0],
          gdal.ExtendedDataType.CreateString(10),
          [f'STRING_FORMAT={string_format}', 'COMPRESS=ZLIB'],
      )
      self.assertEqual(ar.Write(['ab', input_str]), gdal.CE_None)

    options = gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE
    with gdal.OpenEx(filename, options) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), ['ab', output_str])

    gdal.RmdirRecursive(filename)

  def test_zarr_update_array_string(self):
    src_filename = pathlib.Path(self.getTestFilePath('unicode_le.zarr'))

    filename = pathlib.Path('/vsimem/update_array_string.zarr')

    gdal.Mkdir(filename, 0o755)

    data = (src_filename / '.zarray').read_bytes()
    gdal.FileFromMemBuffer(filename / '.zarray', data)
    gdal.FileFromMemBuffer(filename / '0', (src_filename / '0').read_bytes())

    eta = '\u03B7'

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), ['\u00E9'])
      self.assertEqual(ar.Write([eta]), gdal.CE_None)

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), [eta])

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      (gdal.GDT_Int8, 'ZARR_V2'),
      (gdal.GDT_Byte, 'ZARR_V2'),
      (gdal.GDT_Int16, 'ZARR_V2'),
      (gdal.GDT_UInt16, 'ZARR_V2'),
      (gdal.GDT_Int32, 'ZARR_V2'),
      (gdal.GDT_UInt32, 'ZARR_V2'),
      (gdal.GDT_Int64, 'ZARR_V2'),
      (gdal.GDT_UInt64, 'ZARR_V2'),
      # SWIG does not support Float16
      # (gdal.GDT_Float16, 'ZARR_V2'),
      (gdal.GDT_Float32, 'ZARR_V2'),
      (gdal.GDT_Float64, 'ZARR_V2'),
      (gdal.GDT_Int8, 'ZARR_V3'),
      (gdal.GDT_Byte, 'ZARR_V3'),
      (gdal.GDT_Int16, 'ZARR_V3'),
      (gdal.GDT_UInt16, 'ZARR_V3'),
      (gdal.GDT_Int32, 'ZARR_V3'),
      (gdal.GDT_UInt32, 'ZARR_V3'),
      (gdal.GDT_Int64, 'ZARR_V3'),
      (gdal.GDT_UInt64, 'ZARR_V3'),
      # SWIG does not support Float16
      # (gdal.GDT_Float16, 'ZARR_V3'),
      (gdal.GDT_Float32, 'ZARR_V3'),
      (gdal.GDT_Float64, 'ZARR_V3'),
  ])
  def test_zarr_create_fortran_order_3d_and_compression_and_dim_separator(
      self, gdal_data_type: int, zarr_format: str
  ):
    filename = pathlib.Path(
        f'/vsimem/3d_dim_separator_{gdal_data_type}_{zarr_format}.zarr'
    )
    array_type = _gdal_data_type_to_array_type[gdal_data_type]

    def create():
      options = [f'FORMAT={zarr_format}']
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim1 = rg.CreateDimension('dim1', None, None, 3)
      dim2 = rg.CreateDimension('dim2', None, None, 4)

      ar = rg.CreateMDArray(
          'test',
          [dim0, dim1, dim2],
          gdal.ExtendedDataType.Create(gdal_data_type),
          ['CHUNK_MEMORY_LAYOUT=F', 'COMPRESS=gzip', 'DIM_SEPARATOR=/'],
      )
      self.assertEqual(
          ar.Write(array.array(array_type, [i for i in range(2 * 3 * 4)])),
          gdal.CE_None,
      )

    create()

    if zarr_format == 'ZARR_V2':
      f = gdal.VSIFOpenL(filename / 'test/.zarray', 'rb')
    else:
      f = gdal.VSIFOpenL(filename / 'test/zarr.json', 'rb')
    data = gdal.VSIFReadL(1, 10000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    if zarr_format == 'ZARR_V2':
      self.assertIn('order', j)
      self.assertEqual(j['order'], 'F')
    else:
      self.assertIn('codecs', j)
      self.assertEqual(
          j['codecs'],
          [
              {'name': 'transpose', 'configuration': {'order': 'F'}},
              {'name': 'bytes', 'configuration': {'endian': 'little'}},
              {'name': 'gzip', 'configuration': {'level': 6}},
          ],
      )

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(
          ar.Read(), array.array(array_type, [i for i in range(2 * 3 * 4)])
      )

    gdal.RmdirRecursive(filename)

  def test_zarr_create_unit_offset_scale(self):
    filename = pathlib.Path('/vsimem/test_zarr_create_unit_offset_scale.zarr')

    def create():
      ds = gdal.GetDriverByName('ZARR').CreateMultiDimensional(filename)
      rg = ds.GetRootGroup()

      ar = rg.CreateMDArray(
          'test', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      self.assertEqual(ar.SetOffset(1.5), gdal.CE_None)
      self.assertEqual(ar.SetScale(2.5), gdal.CE_None)
      self.assertEqual(ar.SetUnit('my unit'), gdal.CE_None)

    create()

    f = gdal.VSIFOpenL(filename / 'test/.zattrs', 'rb')
    data = gdal.VSIFReadL(1, 10000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    self.assertIn('add_offset', j)
    self.assertEqual(j['add_offset'], 1.5)
    self.assertIn('scale_factor', j)
    self.assertEqual(j['scale_factor'], 2.5)
    self.assertIn('units', j)
    self.assertEqual(j['units'], 'my unit')

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.GetOffset(), 1.5)
      self.assertEqual(ar.GetScale(), 2.5)
      self.assertEqual(ar.GetUnit(), 'my unit')

    gdal.RmdirRecursive(filename)

  def test_zarr_getcoordinatevariables(self):
    src_filename = pathlib.Path(
        gdrivers_util.GetTestFilePath('netcdf/expanded_form_of_grid_mapping.nc')
    )
    src_ds = gdal.OpenEx(src_filename, gdal.OF_MULTIDIM_RASTER)

    filename = pathlib.Path('/vsimem/test_zarr_create_unit_offset_scale.zarr')
    with gdal.MultiDimTranslate(filename, src_ds, format='Zarr') as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('temp')
      coordinate_vars = ar.GetCoordinateVariables()

      self.assertLen(coordinate_vars, 2)
      self.assertEqual(coordinate_vars[0].GetName(), 'lat')
      self.assertEqual(coordinate_vars[1].GetName(), 'lon')

      self.assertEmpty(coordinate_vars[0].GetCoordinateVariables())

    gdal.RmdirRecursive(filename)

  def test_zarr_create_copy(self):
    filename = gdrivers_util.GetTestFilePath('gtiff/uint16.tif')
    self.CheckOpen(filename, check_driver=False)
    self.CheckCreateCopy(vsimem=True, remove_result=True)

  def test_zarr_create(self):
    zarr_format = 'ZARR_V2'
    filename = pathlib.Path(f'/vsimem/test_zarr_create_{zarr_format}.zarr')
    options = ['ARRAY_NAME=foo', 'FORMAT={zarr_format}', 'SINGLE_ARRAY=NO']
    with self.driver.Create(filename, 1, 1, 3, options=options) as ds:
      self.assertIsNone(ds.GetGeoTransform(can_return_null=True))
      self.assertIsNone(ds.GetSpatialRef())
      self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())
      self.assertEqual(ds.GetRasterBand(1).SetNoDataValue(10), gdal.CE_None)
      self.assertIsNone(ds.GetRasterBand(1).GetOffset())
      self.assertEqual(ds.GetRasterBand(1).SetOffset(1.5), gdal.CE_None)
      self.assertIsNone(ds.GetRasterBand(1).GetScale())
      self.assertEqual(ds.GetRasterBand(1).SetScale(2.5), gdal.CE_None)
      self.assertEmpty(ds.GetRasterBand(1).GetUnitType())
      self.assertEqual(ds.GetRasterBand(1).SetUnitType('my_unit'), gdal.CE_None)
      self.assertEqual(ds.SetMetadata({'FOO': 'BAR'}), gdal.CE_None)

    with gdal.Open(f'ZARR:{filename}:/foo_band1') as ds:
      self.assertEqual(ds.GetMetadata(), {'FOO': 'BAR'})
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 10.0)
      self.assertEqual(ds.GetRasterBand(1).GetOffset(), 1.5)

    gdal.RmdirRecursive(filename)

  def test_zarr_create_append_subdataset(self):
    filename = pathlib.Path('/vsimem/test_zarr_create_append_subdataset.zarr')

    def create():
      with self.driver.Create(
          filename, 3, 2, 1, options=['ARRAY_NAME=foo']
      ) as ds:
        ds.SetGeoTransform([2, 1, 0, 49, 0, -1])

      # Same dimensions. Will reuse the ones of foo
      options = ['APPEND_SUBDATASET=YES', 'ARRAY_NAME=bar']
      with self.driver.Create(filename, 3, 2, 1, options=options) as ds:
        ds.SetGeoTransform([2, 1, 0, 49, 0, -1])

      # Different dimensions.
      options = ['APPEND_SUBDATASET=YES', 'ARRAY_NAME=baz']
      ds = self.driver.Create(filename, 30, 20, 1, options=options)
      ds.SetGeoTransform([2, 0.1, 0, 49, 0, -0.1])

    create()

    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()

    foo = rg.OpenMDArray('foo')
    self.assertEqual(foo.GetDimensions()[0].GetName(), 'Y')
    self.assertEqual(foo.GetDimensions()[1].GetName(), 'X')

    bar = rg.OpenMDArray('bar')
    self.assertEqual(bar.GetDimensions()[0].GetName(), 'Y')
    self.assertEqual(bar.GetDimensions()[1].GetName(), 'X')

    baz = rg.OpenMDArray('baz')
    self.assertEqual(baz.GetDimensions()[0].GetName(), 'baz_Y')
    self.assertEqual(baz.GetDimensions()[1].GetName(), 'baz_X')

    gdal.RmdirRecursive(filename)

  def test_zarr_create_array_invalid_blocksize(self):
    blocksize = '1,2'

    filename = pathlib.Path(f'/vsimem/array_invalid_blocksize{blocksize}.zarr')

    with self.driver.CreateMultiDimensional(filename) as ds:
      rg = ds.GetRootGroup()
      dimensions = [
          rg.CreateDimension('dim0', None, None, 2),
          rg.CreateDimension('dim1', None, None, 2),
          rg.CreateDimension('dim2', None, None, 2),
      ]
      data_type = gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      options = ['BLOCKSIZE=' + blocksize]

      message = 'Invalid number of values in BLOCKSIZE'
      with self.assertRaisesRegex(RuntimeError, message):
        rg.CreateMDArray('test', dimensions, data_type, options)

    gdal.RmdirRecursive(filename)

  def test_zarr_read_filters(self):
    filename = self.getTestFilePath('delta_filter_i4.zarr')
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('i', [i for i in range(10)]))

  def test_zarr_update_with_filters(self):
    src_filename = pathlib.Path(self.getTestFilePath('delta_filter_i4.zarr'))
    filename = pathlib.Path('/vsimem/update_with_filters.zarr')

    gdal.Mkdir(filename, 0o755)

    data = (src_filename / '.zarray').read_bytes()
    gdal.FileFromMemBuffer(filename / '.zarray', data)
    gdal.FileFromMemBuffer(filename / '0', (src_filename / '0').read_bytes())

    options = gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE
    with gdal.OpenEx(filename, options) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('i', [i for i in range(10)]))
      self.assertEqual(
          ar.Write(array.array('i', [10 - i for i in range(10)])), gdal.CE_None
      )

    options = gdal.OF_MULTIDIM_RASTER
    with gdal.OpenEx(filename, options) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.Read(), array.array('i', [10 - i for i in range(10)]))

    gdal.RmdirRecursive(filename)

  def test_zarr_create_with_filter(self):
    filename = pathlib.Path('/vsimem/create_with_filter.zarr')

    def create():
      src_filename = gdrivers_util.GetTestFilePath('gtiff/uint16.tif')
      out_bands = 1
      band = 1

      src_ds = gdal.Open(src_filename, gdal.GA_ReadOnly)
      xsize = src_ds.RasterXSize
      ysize = src_ds.RasterYSize
      src_img = src_ds.GetRasterBand(band).ReadRaster(0, 0, xsize, ysize)
      minmax = src_ds.GetRasterBand(band).ComputeRasterMinMax()

      ds = self.driver.Create(
          filename,
          xsize,
          ysize,
          out_bands,
          src_ds.GetRasterBand(band).DataType,
          options=['FILTER=delta'],
      )

      ds.GetRasterBand(band).WriteRaster(0, 0, xsize, ysize, src_img)
      self.assertEqual(ds.GetRasterBand(band).Checksum(), 4672)
      self.assertEqual(ds.GetRasterBand(band).ComputeRasterMinMax(), minmax)

    create()

    f = gdal.VSIFOpenL(filename / 'create_with_filter/.zarray', 'rb')
    data = gdal.VSIFReadL(1, 10000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    self.assertIn('filters', j)
    self.assertEqual(j['filters'], [{'id': 'delta', 'dtype': '<u2'}])

    gdal.RmdirRecursive(filename)

  def test_zarr_pam_spatial_ref(self):
    filename = pathlib.Path('/vsimem/pam_spatial_ref.zarr')

    def create():
      ds = self.driver.CreateMultiDimensional(filename)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim1 = rg.CreateDimension('dim1', None, None, 2)
      rg.CreateMDArray(
          'test', [dim0, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )

    create()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertIsNone(ar.GetSpatialRef())

    def set_crs():
      # Open in read-only.
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      crs = osr.SpatialReference()
      crs.ImportFromEPSG(4326)
      # Latitude first.
      crs.SetDataAxisToSRSAxisMapping([1, 2])
      crs.SetCoordinateEpoch(2021.2)
      self.assertEqual(ar.SetSpatialRef(crs), gdal.CE_None)

    set_crs()

    f = gdal.VSIFOpenL(filename / 'pam.aux.xml', 'rb+')
    data = gdal.VSIFReadL(1, 1000, f).decode('utf-8')
    self.assertTrue(data.endswith('</PAMDataset>\n'))
    data = data[0 : -len('</PAMDataset>\n')] + '<Other/></PAMDataset>\n'
    gdal.VSIFSeekL(f, 0, 0)
    gdal.VSIFWriteL(data, 1, len(data), f)
    gdal.VSIFCloseL(f)

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      crs = ar.GetSpatialRef()
      self.assertEqual(crs.GetAuthorityCode(None), '4326')
      self.assertEqual(crs.GetDataAxisToSRSAxisMapping(), [1, 2])
      self.assertEqual(crs.GetCoordinateEpoch(), 2021.2)

    # Check classic dataset.
    with gdal.Open(filename) as ds:
      crs = ds.GetSpatialRef()
      self.assertEqual(crs.GetAuthorityCode(None), '4326')
      self.assertEqual(crs.GetDataAxisToSRSAxisMapping(), [2, 1])

    def unset_crs():
      # Open in read-only
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      self.assertEqual(ar.SetSpatialRef(None), gdal.CE_None)

    unset_crs()

    f = gdal.VSIFOpenL(filename / 'pam.aux.xml', 'rb')
    data = gdal.VSIFReadL(1, 1000, f).decode('utf-8')
    gdal.VSIFCloseL(f)
    self.assertIn('<Other />', data)

    # Check unset CRS.
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray(rg.GetMDArrayNames()[0])
      crs = ar.GetSpatialRef()
      self.assertIsNone(crs)

    gdal.RmdirRecursive(filename)

  def test_zarr_read_too_large_tile_size(self):
    filename = pathlib.Path('/vsimem/test_zarr_read_too_large_tile_size.zarr')
    j = {
        'chunks': [1000000, 2000],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [5, 4],
        'zarr_format': 2,
    }

    gdal.Mkdir(filename, 0o755)
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      with self.assertRaisesRegex(RuntimeError, 'Array test does not exist'):
        ds.GetRootGroup().OpenMDArray('test').Read()

    gdal.RmdirRecursive(filename)

  def test_zarr_read_recursive_array_loading(self):
    filename = pathlib.Path('/vsimem/read_recursive_array_loading.zarr')

    gdal.Mkdir(filename, 0o755)
    j = {'zarr_format': 2}
    gdal.FileFromMemBuffer(filename / '.zgroup', json.dumps(j))

    j = {
        'chunks': [1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [1],
        'zarr_format': 2,
    }
    gdal.FileFromMemBuffer(filename / 'a/.zarray', json.dumps(j))
    gdal.FileFromMemBuffer(filename / 'b/.zarray', json.dumps(j))

    j = {'_ARRAY_DIMENSIONS': ['b']}
    gdal.FileFromMemBuffer(filename / 'a/.zattrs', json.dumps(j))

    j = {'_ARRAY_DIMENSIONS': ['a']}
    gdal.FileFromMemBuffer(filename / 'b/.zattrs', json.dumps(j))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      message = f'Attempt at recursively loading {filename}'
      with self.assertRaisesRegex(RuntimeError, message):
        ds.GetRootGroup().OpenMDArray('a')

    gdal.RmdirRecursive(filename)

  def test_zarr_read_too_deep_array_loading(self):
    filename = pathlib.Path('/vsimem/read_recursive_array_loading.zarr')
    gdal.Mkdir(filename, 0o755)

    j = {'zarr_format': 2}
    gdal.FileFromMemBuffer(filename / '.zgroup', json.dumps(j))

    j = {
        'chunks': [1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [1],
        'zarr_format': 2,
    }

    depth = 33
    for i in range(depth):
      gdal.FileFromMemBuffer(filename / f'{i}/.zarray', json.dumps(j))

    for i in range(depth - 1):
      j = {'_ARRAY_DIMENSIONS': [f'{i+1}']}
      gdal.FileFromMemBuffer(filename / f'{i}/.zattrs', json.dumps(j))

    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    self.assertIsNotNone(ds)
    with self.assertRaisesRegex(
        RuntimeError, 'Too deep call stack in LoadArray()'
    ):
      ds.GetRootGroup().OpenMDArray('0')

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ('nczarr_v2.zarr', '/MyGroup/Group_A'),
      ('nczarr_v2.zarr/MyGroup', '/Group_A'),
      ('nczarr_v2.zarr/MyGroup/Group_A', ''),
      ('nczarr_v2.zarr/MyGroup/Group_A/dset2', None),
  ])
  def test_zarr_read_nczarr_v2(self, filename_tail, path):
    filename = self.getTestFilePath(filename_tail)
    with self.assertRaisesRegex(
        RuntimeError, 'Update of NCZarr datasets is not supported'
    ):
      gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)

    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()

    ar = rg.OpenMDArrayFromFullname((path if path else '') + '/dset2')
    dims = ar.GetDimensions()
    self.assertLen(dims, 2)
    self.assertEqual(dims[0].GetSize(), 3)
    self.assertEqual(dims[0].GetName(), 'lat')
    self.assertEqual(dims[0].GetFullName(), '/MyGroup/lat')
    self.assertEqual(dims[0].GetIndexingVariable().GetName(), 'lat')
    self.assertEqual(dims[0].GetType(), gdal.DIM_TYPE_HORIZONTAL_Y)
    self.assertEqual(dims[0].GetDirection(), 'NORTH')

    self.assertEqual(dims[1].GetSize(), 3)
    self.assertEqual(dims[1].GetName(), 'lon')
    self.assertEqual(dims[1].GetFullName(), '/MyGroup/lon')
    self.assertEqual(dims[1].GetIndexingVariable().GetName(), 'lon')
    self.assertEqual(dims[1].GetType(), gdal.DIM_TYPE_HORIZONTAL_X)
    self.assertEqual(dims[1].GetDirection(), 'EAST')

    if path:
      ar = rg.OpenMDArrayFromFullname(path + '/dset3')
      dims = ar.GetDimensions()
      self.assertLen(dims, 2)
      self.assertEqual(dims[0].GetSize(), 2)
      self.assertEqual(dims[0].GetName(), 'lat')
      self.assertEqual(dims[0].GetFullName(), '/MyGroup/Group_A/lat')

      self.assertEqual(dims[1].GetSize(), 2)
      self.assertEqual(dims[1].GetName(), 'lon')
      self.assertEqual(dims[1].GetFullName(), '/MyGroup/Group_A/lon')

    if filename.endswith('nczarr_v2.zarr'):
      mygroup = rg.OpenGroup('MyGroup')
      self.assertEqual(mygroup.GetMDArrayNames(), ['lon', 'lat', 'dset1'])

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_cache_tile_presence(self, zarr_format: str):
    # Caching does not happen with /vsimem.
    filename = pathlib.Path(self.create_tempdir()) / 'test.zarr'

    def create():
      options = [f'FORMAT={zarr_format}']
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim1 = rg.CreateDimension('dim1', None, None, 5)
      ar = rg.CreateMDArray(
          'test',
          [dim0, dim1],
          gdal.ExtendedDataType.Create(gdal.GDT_Byte),
          ['BLOCKSIZE=1,2'],
      )
      self.assertEqual(
          ar.Write(
              struct.pack('B' * 1, 10), array_start_idx=[0, 0], count=[1, 1]
          ),
          gdal.CE_None,
      )
      self.assertEqual(
          ar.Write(
              struct.pack('B' * 1, 100), array_start_idx=[1, 3], count=[1, 1]
          ),
          gdal.CE_None,
      )

    create()

    def open_with_cache_tile_presence_option():
      ds = gdal.OpenEx(
          filename,
          gdal.OF_MULTIDIM_RASTER,
          open_options=['CACHE_TILE_PRESENCE=YES'],
      )
      rg = ds.GetRootGroup()
      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        self.assertIsNotNone(rg.OpenMDArray('test'))

    open_with_cache_tile_presence_option()

    # Check that the cache exists
    if zarr_format == 'ZARR_V2':
      cache_filename = filename / 'test/.zarray.gmac'
    else:
      cache_filename = filename / 'test/zarr.json.gmac'
    self.assertIsNotNone(gdal.VSIStatL(cache_filename))

    def read_content():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()
      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = rg.OpenMDArray('test')
      expect = (10, 0, 0, 0, 0, 0, 0, 0, 100, 0)
      self.assertEqual(struct.unpack('B' * 2 * 5, ar.Read()), expect)

    read_content()

    # Do it again.
    open_with_cache_tile_presence_option()
    read_content()

    def alter_cache():
      ds = gdal.OpenEx(cache_filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      self.assertEqual(rg.GetMDArrayNames(), ['_test_tile_presence'])
      ar = rg.OpenMDArray('_test_tile_presence')
      self.assertEqual(
          struct.unpack('B' * 2 * 3, ar.Read()), (1, 0, 0, 0, 1, 0)
      )
      self.assertEqual(
          ar.Write(
              struct.pack('B' * 1, 0), array_start_idx=[1, 1], count=[1, 1]
          ),
          gdal.CE_None,
      )

    alter_cache()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = rg.OpenMDArray('test')
      expect = (10, 0, 0, 0, 0, 0, 0, 0, 0, 0)
      self.assertEqual(struct.unpack('B' * 2 * 5, ar.Read()), expect)

  @parameterized.parameters([
      ('ZARR_V2', 'NONE'),
      ('ZARR_V2', 'GZIP'),
      ('ZARR_V3', 'NONE'),
      ('ZARR_V3', 'GZIP'),
  ])
  def test_zarr_advise_read(self, zarr_format: str, compression: str):
    filename = pathlib.Path(
        f'/vsimem/advise_read_{zarr_format}_{compression}.zarr'
    )

    dim0_size = 1230
    dim1_size = 2570
    dim0_blocksize = 20
    dim1_blocksize = 30
    data_ar = [(i % 256) for i in range(dim0_size * dim1_size)]

    # Create empty block
    y_offset = dim0_blocksize
    x_offset = dim1_blocksize
    for y in range(dim0_blocksize):
      for x in range(dim1_blocksize):
        data_ar[dim1_size * (y + y_offset) + x + x_offset] = 0

    data = array.array('B', data_ar)

    def create():
      options = [f'FORMAT={zarr_format}']
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, dim0_size)
      dim1 = rg.CreateDimension('dim1', None, None, dim1_size)
      ar = rg.CreateMDArray(
          'test',
          [dim0, dim1],
          gdal.ExtendedDataType.Create(gdal.GDT_Byte),
          [
              f'COMPRESS={compression}',
              f'BLOCKSIZE={dim0_blocksize},{dim1_blocksize}',
          ],
      )
      ar.SetNoDataValueDouble(0)
      self.assertEqual(ar.Write(data), gdal.CE_None)

    create()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('test')

      message = 'CACHE_SIZE=1 is not big enough to cache all needed tiles'
      with self.assertRaisesRegex(RuntimeError, message):
        ar.AdviseRead(options=['CACHE_SIZE=1'])

      got_data_before_advise_read = ar.Read(
          array_start_idx=[40, 51],
          count=[2 * dim0_blocksize, 2 * dim1_blocksize],
      )

      self.assertEqual(ar.AdviseRead(), gdal.CE_None)
      self.assertEqual(ar.Read(), data)

      self.assertEqual(
          ar.AdviseRead(
              array_start_idx=[40, 51],
              count=[2 * dim0_blocksize, dim1_blocksize],
          ),
          gdal.CE_None,
      )
      # Read more than AdviseRead() window
      got_data = ar.Read(
          array_start_idx=[40, 51],
          count=[2 * dim0_blocksize, 2 * dim1_blocksize],
      )
      self.assertEqual(got_data, got_data_before_advise_read)

    gdal.RmdirRecursive(filename)

  def test_zarr_read_invalid_nczarr_dim(self):
    filename = pathlib.Path('/vsimem/read_invalid_nczarr_dim.zarr')

    gdal.Mkdir(filename, 0o755)

    j = {
        'chunks': [1, 1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [1, 1],
        'zarr_format': 2,
        '_NCZARR_ARRAY': {'dimrefs': ['/MyGroup/lon', '/OtherGroup/lat']},
    }

    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    j = {
        'chunks': [1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [1],
        'zarr_format': 2,
    }

    gdal.FileFromMemBuffer(filename / 'MyGroup/lon/.zarray', json.dumps(j))

    j = {'_NCZARR_GROUP': {'dims': {'lon': 0}}}

    gdal.FileFromMemBuffer(filename / 'MyGroup/.zgroup', json.dumps(j))

    j = {
        'chunks': [2],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [2],
        'zarr_format': 2,
    }

    gdal.FileFromMemBuffer(filename / 'OtherGroup/lat/.zarray', json.dumps(j))

    j = {'_NCZARR_GROUP': {'dims': {'lat': 2, 'invalid.name': 2}}}

    gdal.FileFromMemBuffer(filename / 'OtherGroup/.zgroup', json.dumps(j))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      self.assertIsNotNone(rg.OpenMDArray('read_invalid_nczarr_dim'))

    gdal.RmdirRecursive(filename)

  def test_zarr_read_nczar_repeated_array_names(self):
    filename = pathlib.Path('/vsimem/read_nczar_repeated_array_names.zarr')

    gdal.Mkdir(filename, 0o755)

    j = {
        '_NCZARR_GROUP': {
            'dims': {'lon': 1},
            'vars': ['a', 'a', 'lon', 'lon'],
            'groups': ['g', 'g'],
        }
    }

    gdal.FileFromMemBuffer(filename / '.zgroup', json.dumps(j))

    j = {
        'chunks': [1, 1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [1, 1],
        'zarr_format': 2,
    }

    gdal.FileFromMemBuffer(filename / 'a/.zarray', json.dumps(j))

    j = {
        'chunks': [1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'C',
        'shape': [1],
        'zarr_format': 2,
    }

    gdal.FileFromMemBuffer(filename / 'lon/.zarray', json.dumps(j))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      self.assertEqual(rg.GetMDArrayNames(), ['lon', 'a'])
      self.assertIsNotNone(rg.OpenMDArray('a'))
      self.assertEqual(rg.GetGroupNames(), ['g'])

    gdal.RmdirRecursive(filename)

  def test_zarr_read_test_overflow_in_AllocateWorkingBuffers_due_to_fortran(
      self,
  ):
    filename = pathlib.Path('/vsimem/AllocateWorkingBuffers_fortran.zarr')

    gdal.Mkdir(filename, 0o755)
    j = {
        'chunks': [(1 << 32) - 1, (1 << 32) - 1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'F',
        'shape': [1, 1],
        'zarr_format': 2,
    }

    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))
    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('AllocateWorkingBuffers_fortran')
      with self.assertRaisesRegex(RuntimeError, 'Too large chunk size'):
        ar.Read(count=[1, 1])

    gdal.RmdirRecursive(filename)

  def test_zarr_read_test_overflow_in_AllocateWorkingBuffers_due_to_type_change(
      self,
  ):
    filename = pathlib.Path('/vsimem/AllocateWorkingBuffers_type_change.zarr')

    gdal.Mkdir(filename, 0o755)
    j = {
        'chunks': [(1 << 32) - 1, (1 << 32) - 1],
        'compressor': None,
        'dtype': '!b1',
        'fill_value': None,
        'filters': None,
        'order': 'F',
        'shape': [1, 1],
        'zarr_format': 2,
    }

    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      with self.assertRaisesRegex(RuntimeError, 'Array test does not exist'):
        rg.OpenMDArray('test')
      # Cannot do the following with exceptions enabled.
      # ar.Read(count=[1, 1])

    gdal.RmdirRecursive(filename)

  def test_zarr_read_do_not_crash_on_invalid_byteswap_on_ascii_string(self):
    filename = pathlib.Path('/vsimem/invalid_byteswap_on_ascii_string.zarr')
    gdal.Mkdir(filename, 0o755)

    j = {
        'chunks': [1],
        'compressor': None,
        'dtype': [['x', '>S2']],  # byteswap here is not really valid...
        'fill_value': base64.b64encode(b'XX').decode('utf-8'),
        'filters': None,
        'order': 'C',
        'shape': [1],
        'zarr_format': 2,
    }
    gdal.FileFromMemBuffer(filename / '.zarray', json.dumps(j))

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      with self.assertRaisesRegex(RuntimeError, 'Array test does not exist'):
        rg.OpenMDArray('test')

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_resize(self, zarr_format: str, create_z_metadata: str):
    filename = pathlib.Path(
        f'/vsimem/resize_{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim1 = rg.CreateDimension('dim1', None, None, 2)
      var = rg.CreateMDArray(
          'test', [dim0, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      self.assertEqual(
          var.Write(struct.pack('B' * (2 * 2), 1, 2, 3, 4)), gdal.CE_None
      )

    create()

    # Resize read only.
    ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
    rg = ds.GetRootGroup()
    # Triggers: Warning 1: fill_value = null is invalid.
    with gdal.quiet_warnings():
      var = rg.OpenMDArray('test')

    message = r'Resize\(\) not supported on read-only file'
    with self.assertRaisesRegex(RuntimeError, message):
      var.Resize([5, 2])

    def resize():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      with gdal.quiet_warnings():
        var = rg.OpenMDArray('test')

      message = r'Resize\(\) does not support shrinking the array'
      with self.assertRaisesRegex(RuntimeError, message):
        var.Resize([1, 2])

      self.assertEqual(var.Resize([5, 2]), gdal.CE_None)
      self.assertEqual(var.GetDimensions()[0].GetSize(), 5)
      self.assertEqual(var.GetDimensions()[1].GetSize(), 2)
      self.assertEqual(
          var.Write(
              struct.pack('B' * (3 * 2), 5, 6, 7, 8, 9, 10),
              array_start_idx=[2, 0],
              count=[3, 2],
          ),
          gdal.CE_None,
      )

    resize()

    if create_z_metadata == 'YES':
      f = gdal.VSIFOpenL(filename / '.zmetadata', 'rb')
      data = gdal.VSIFReadL(1, 10000, f)
      gdal.VSIFCloseL(f)
      j = json.loads(data)
      self.assertEqual(j['metadata']['test/.zarray']['shape'], [5, 2])

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      with gdal.quiet_warnings():
        var = rg.OpenMDArray('test')
      self.assertEqual(var.GetDimensions()[0].GetSize(), 5)
      self.assertEqual(var.GetDimensions()[1].GetSize(), 2)
      expect = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
      self.assertEqual(struct.unpack('B' * (5 * 2), var.Read()), expect)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([True, False])
  def test_zarr_resize_XARRAY(self, create_z_metadata: bool):
    filename = pathlib.Path(f'/vsimem/resize_XARRAY_{create_z_metadata}.zarr')

    def create():
      options = ['CREATE_ZMETADATA=' + ('YES' if create_z_metadata else 'NO')]
      ds = self.driver.CreateMultiDimensional(filename, options)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim0_var = rg.CreateMDArray(
          'dim0', [dim0], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      dim0.SetIndexingVariable(dim0_var)
      dim1 = rg.CreateDimension('dim1', None, None, 2)
      dim1_var = rg.CreateMDArray(
          'dim1', [dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      dim1.SetIndexingVariable(dim1_var)
      var = rg.CreateMDArray(
          'test', [dim0, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      self.assertEqual(
          var.Write(struct.pack('B' * (2 * 2), 1, 2, 3, 4)), gdal.CE_None
      )

      self.assertIsNotNone(
          rg.CreateMDArray(
              'test2', [dim0, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
          )
      )

    create()

    def resize():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      var = rg.OpenMDArray('test')
      message = r'Resize\(\) does not support shrinking the array'
      with self.assertRaisesRegex(RuntimeError, message):
        var.Resize([1, 2])

      dim0 = rg.OpenMDArray('dim0')

      self.assertEqual(var.Resize([5, 2]), gdal.CE_None)
      self.assertEqual(var.GetDimensions()[0].GetSize(), 5)
      self.assertEqual(var.GetDimensions()[1].GetSize(), 2)
      self.assertEqual(
          var.Write(
              struct.pack('B' * (3 * 2), 5, 6, 7, 8, 9, 10),
              array_start_idx=[2, 0],
              count=[3, 2],
          ),
          gdal.CE_None,
      )

      self.assertEqual(dim0.GetDimensions()[0].GetSize(), 5)

    resize()

    if create_z_metadata:
      f = gdal.VSIFOpenL(filename / '.zmetadata', 'rb')
      data = gdal.VSIFReadL(1, 10000, f)
      gdal.VSIFCloseL(f)
      j = json.loads(data)
      self.assertEqual(j['metadata']['test/.zarray']['shape'], [5, 2])
      self.assertEqual(j['metadata']['dim0/.zarray']['shape'], [5])

      with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
        rg = ds.GetRootGroup()
        var = rg.OpenMDArray('test')
        self.assertEqual(var.GetDimensions()[0].GetSize(), 5)
        self.assertEqual(var.GetDimensions()[1].GetSize(), 2)
        expect = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
        self.assertEqual(struct.unpack('B' * (5 * 2), var.Read()), expect)

        dim0 = rg.OpenMDArray('dim0')
        self.assertEqual(dim0.GetDimensions()[0].GetSize(), 5)

        var2 = rg.OpenMDArray('test')
        self.assertEqual(var2.GetDimensions()[0].GetSize(), 5)

    gdal.RmdirRecursive(filename)

  def test_zarr_resize_dim_referenced_twice(self):
    filename = pathlib.Path('/vsimem/resize_dim_referenced_twice.zarr')

    def create():
      ds = gdal.GetDriverByName('ZARR').CreateMultiDimensional(filename)
      rg = ds.GetRootGroup()

      dim0 = rg.CreateDimension('dim0', None, None, 2)
      dim0_var = rg.CreateMDArray(
          'dim0', [dim0], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      dim0.SetIndexingVariable(dim0_var)

      var = rg.CreateMDArray(
          'test', [dim0, dim0], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      self.assertEqual(
          var.Write(struct.pack('B' * (2 * 2), 1, 2, 3, 4)), gdal.CE_None
      )

    create()

    def resize():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      var = rg.OpenMDArray('test')

      message = (
          'Cannot resize a dimension referenced several times to different'
          ' sizes'
      )
      with self.assertRaisesRegex(RuntimeError, message):
        var.Resize([3, 4])
      with self.assertRaisesRegex(RuntimeError, message):
        var.Resize([4, 3])

      self.assertEqual(var.Resize([3, 3]), gdal.CE_None)
      self.assertEqual(var.GetDimensions()[0].GetSize(), 3)
      self.assertEqual(var.GetDimensions()[1].GetSize(), 3)

    resize()

    f = gdal.VSIFOpenL(filename / '.zmetadata', 'rb')
    data = gdal.VSIFReadL(1, 10000, f)
    gdal.VSIFCloseL(f)
    j = json.loads(data)
    self.assertEqual(j['metadata']['test/.zarray']['shape'], [3, 3])
    self.assertEqual(j['metadata']['dim0/.zarray']['shape'], [3])

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      var = rg.OpenMDArray('test')
      self.assertEqual(var.GetDimensions()[0].GetSize(), 3)
      self.assertEqual(var.GetDimensions()[1].GetSize(), 3)

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_group_at_creation(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        f'/vsimem/rename_group_at_creation_{zarr_format}_{create_z_metadata}'
        '.zarr'
    )

    def test():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      group = rg.CreateGroup('group')
      group_attr = group.CreateAttribute(
          'group_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      rg.CreateGroup('other_group')
      dim = group.CreateDimension(
          'dim0', 'unspecified type', 'unspecified direction', 2
      )
      ar = group.CreateMDArray(
          'ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )

      subgroup = group.CreateGroup('subgroup')
      subgroup_attr = subgroup.CreateAttribute(
          'subgroup_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      subgroup_ar = subgroup.CreateMDArray(
          'subgroup_ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      subgroup_ar_attr = subgroup_ar.CreateAttribute(
          'subgroup_ar_attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )

      with self.assertRaisesRegex(RuntimeError, 'Cannot rename root group'):
        rg.Rename('foo')

      with self.assertRaisesRegex(RuntimeError, 'Invalid group name'):
        group.Rename('')

      message = 'A group with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        group.Rename('other_group')

      message = 'An array with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        subgroup.Rename('ar')

      group.Rename('group_renamed')
      self.assertEqual(group.GetName(), 'group_renamed')
      self.assertEqual(group.GetFullName(), '/group_renamed')

      self.assertEqual(
          set(rg.GetGroupNames()), {'group_renamed', 'other_group'}
      )

      self.assertEqual(dim.GetName(), 'dim0')
      self.assertEqual(dim.GetFullName(), '/group_renamed/dim0')

      self.assertEqual(group_attr.GetName(), 'group_attr')
      self.assertEqual(
          group_attr.GetFullName(), '/group_renamed/_GLOBAL_/group_attr'
      )

      self.assertEqual(ar.GetName(), 'ar')
      self.assertEqual(ar.GetFullName(), '/group_renamed/ar')

      self.assertEqual(attr.GetName(), 'attr')
      self.assertEqual(attr.GetFullName(), '/group_renamed/ar/attr')

      self.assertEqual(subgroup.GetName(), 'subgroup')
      self.assertEqual(subgroup.GetFullName(), '/group_renamed/subgroup')

      self.assertEqual(subgroup_attr.GetName(), 'subgroup_attr')
      self.assertEqual(
          subgroup_attr.GetFullName(),
          '/group_renamed/subgroup/_GLOBAL_/subgroup_attr',
      )

      self.assertEqual(subgroup_ar.GetName(), 'subgroup_ar')
      self.assertEqual(
          subgroup_ar.GetFullName(), '/group_renamed/subgroup/subgroup_ar'
      )

      self.assertEqual(subgroup_ar_attr.GetName(), 'subgroup_ar_attr')
      self.assertEqual(
          subgroup_ar_attr.GetFullName(),
          '/group_renamed/subgroup/subgroup_ar/subgroup_ar_attr',
      )

    test()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()

      self.assertEqual(
          set(rg.GetGroupNames()), {'group_renamed', 'other_group'}
      )

      group = rg.OpenGroup('group_renamed')
      self.assertEqual(
          set([attr.GetName() for attr in group.GetAttributes()]),
          {'group_attr'},
      )

      self.assertEqual(group.GetMDArrayNames(), ['ar'])

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        group.Rename('group_renamed2')

      self.assertEqual(
          set(rg.GetGroupNames()), {'group_renamed', 'other_group'}
      )

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_group_after_reopening(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        '/vsimem/rename_group_after_reopening_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      group = rg.CreateGroup('group')
      group_attr = group.CreateAttribute(
          'group_attr', [], gdal.ExtendedDataType.CreateString()
      )
      group_attr.Write('my_string')
      rg.CreateGroup('other_group')
      dim = group.CreateDimension(
          'dim0', 'unspecified type', 'unspecified direction', 2
      )
      ar = group.CreateMDArray(
          'ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.CreateString()
      )
      attr.Write('foo')
      attr2 = ar.CreateAttribute(
          'attr2', [], gdal.ExtendedDataType.CreateString()
      )
      attr2.Write('foo2')

      group.CreateGroup('subgroup')

    create()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        group.Rename('group_renamed2')

      self.assertEqual(set(rg.GetGroupNames()), {'group', 'other_group'})

    def rename():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      with self.assertRaisesRegex(RuntimeError, 'Cannot rename root group'):
        rg.Rename('foo')

      with self.assertRaisesRegex(RuntimeError, 'Invalid group name'):
        group.Rename('')

      message = 'A group with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        group.Rename('other_group')

      group_attr = group.GetAttribute('group_attr')
      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = group.OpenMDArray('ar')
      attr = ar.GetAttribute('attr')
      attr.Write('bar')

      # Rename group and test effects
      group.Rename('group_renamed')
      self.assertEqual(group.GetName(), 'group_renamed')
      self.assertEqual(group.GetFullName(), '/group_renamed')

      self.assertEqual(
          set(rg.GetGroupNames()), {'group_renamed', 'other_group'}
      )

      self.assertEqual(
          group_attr.GetFullName(), '/group_renamed/_GLOBAL_/group_attr'
      )

      attr2 = ar.GetAttribute('attr2')
      attr2.Write('bar2')

    rename()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()

      group = rg.OpenGroup('group_renamed')
      self.assertEqual(
          set([attr.GetName() for attr in group.GetAttributes()]),
          {'group_attr'},
      )

      self.assertEqual(group.GetMDArrayNames(), ['ar'])

      self.assertEqual(
          set(rg.GetGroupNames()), {'group_renamed', 'other_group'}
      )

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = group.OpenMDArray('ar')

      attr = ar.GetAttribute('attr')
      self.assertEqual(attr.Read(), 'bar')

      attr2 = ar.GetAttribute('attr2')
      self.assertEqual(attr2.Read(), 'bar2')

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_array_at_creation(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        '/vsimem/multidim_rename_array_at_creation_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def test():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      group = rg.CreateGroup('group')
      group.CreateGroup('subgroup')

      dim = group.CreateDimension(
          'dim0', 'unspecified type', 'unspecified direction', 2
      )
      ar = group.CreateMDArray(
          'ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      group.CreateMDArray(
          'other_ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )

      with self.assertRaisesRegex(RuntimeError, 'Invalid array name'):
        ar.Rename('')

      message = 'An array with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        ar.Rename('other_ar')

      message = 'A group with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        ar.Rename('subgroup')

      # Rename array and test effects
      ar.Rename('ar_renamed')

      if zarr_format == 'ZARR_V2':
        self.assertIsNone(gdal.VSIStatL(filename / 'group/ar/.zarray'))
        self.assertIsNotNone(
            gdal.VSIStatL(filename / 'group/ar_renamed/.zarray')
        )
      else:
        self.assertIsNone(gdal.VSIStatL(filename / 'group/ar/zarr.json'))
        self.assertIsNotNone(
            gdal.VSIStatL(filename / 'group/ar_renamed/zarr.json')
        )

      self.assertEqual(ar.GetName(), 'ar_renamed')
      self.assertEqual(ar.GetFullName(), '/group/ar_renamed')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar_renamed', 'other_ar'})

      with self.assertRaisesRegex(RuntimeError, 'Array ar does not exist'):
        group.OpenMDArray('ar')

      self.assertIsNotNone(group.OpenMDArray('ar_renamed'))

      self.assertEqual(attr.GetName(), 'attr')
      self.assertEqual(attr.GetFullName(), '/group/ar_renamed/attr')

    test()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar_renamed', 'other_ar'})

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar_renamed = group.OpenMDArray('ar_renamed')
      self.assertEqual(
          set([attr.GetName() for attr in ar_renamed.GetAttributes()]), {'attr'}
      )

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        ar_renamed.Rename('ar_renamed2')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar_renamed', 'other_ar'})

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_array_after_reopening(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        '/vsimem/multidim_rename_array_after_reopening_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      group = rg.CreateGroup('group')

      dim = group.CreateDimension(
          'dim0', 'unspecified type', 'unspecified direction', 2
      )
      ar = group.CreateMDArray(
          'ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      group.CreateMDArray(
          'other_ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.CreateString()
      )
      attr.Write('foo')

    create()

    def reopen_readonly():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar', 'other_ar'})

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = group.OpenMDArray('ar')

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        ar.Rename('ar_renamed')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar', 'other_ar'})

    reopen_readonly()

    def rename():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar', 'other_ar'})

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = group.OpenMDArray('ar')
      attr = ar.GetAttribute('attr')

      ar.Rename('ar_renamed')

      if zarr_format == 'ZARR_V2':
        self.assertIsNone(gdal.VSIStatL(filename / 'group/ar/.zarray'))
        self.assertIsNotNone(
            gdal.VSIStatL(filename / 'group/ar_renamed/.zarray')
        )
      else:
        self.assertIsNone(gdal.VSIStatL(filename / 'group/ar/zarr.json'))
        self.assertIsNotNone(
            gdal.VSIStatL(filename / 'group/ar_renamed/zarr.json')
        )

      self.assertEqual(ar.GetName(), 'ar_renamed')
      self.assertEqual(ar.GetFullName(), '/group/ar_renamed')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar_renamed', 'other_ar'})

      with self.assertRaisesRegex(RuntimeError, 'Array ar does not exist'):
        group.OpenMDArray('ar')

      self.assertIsNotNone(group.OpenMDArray('ar_renamed'))

      self.assertEqual(attr.GetName(), 'attr')
      self.assertEqual(attr.GetFullName(), '/group/ar_renamed/attr')

      attr.Write('bar')

    rename()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      self.assertEqual(set(group.GetMDArrayNames()), {'ar_renamed', 'other_ar'})

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = group.OpenMDArray('ar_renamed')
      attr = ar.GetAttribute('attr')
      self.assertEqual(attr.Read(), 'bar')

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_attr_after_reopening(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        '/vsimem/multidim_rename_attr_after_reopening_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      group = rg.CreateGroup('group')
      group_attr = group.CreateAttribute(
          'group_attr', [], gdal.ExtendedDataType.CreateString()
      )
      group_attr.Write('foo')

      dim = group.CreateDimension(
          'dim0', 'unspecified type', 'unspecified direction', 2
      )
      ar = group.CreateMDArray(
          'ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      group.CreateMDArray(
          'other_ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.CreateString()
      )
      attr.Write('foo')

    create()

    def rename():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      group = rg.OpenGroup('group')

      # Rename group attribute and test effects
      group_attr = group.GetAttribute('group_attr')
      group_attr.Rename('group_attr_renamed')

      self.assertEqual(group_attr.GetName(), 'group_attr_renamed')
      self.assertEqual(
          group_attr.GetFullName(), '/group/_GLOBAL_/group_attr_renamed'
      )

      group_attr.Write('bar')

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        ar = group.OpenMDArray('ar')
      attr = ar.GetAttribute('attr')

      # Rename attribute and test effects
      attr.Rename('attr_renamed')

      self.assertEqual(attr.GetName(), 'attr_renamed')
      self.assertEqual(attr.GetFullName(), '/group/ar/attr_renamed')

      attr.Write('bar')

    rename()

    def reopen_after_rename():
      with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
        rg = ds.GetRootGroup()
        group = rg.OpenGroup('group')

        group_attr_renamed = group.GetAttribute('group_attr_renamed')
        self.assertEqual(group_attr_renamed.Read(), 'bar')

        with gdal.quiet_warnings():
          # Triggers this for Zarr V3:
          #   Warning 1: fill_value = null is invalid
          ar = group.OpenMDArray('ar')
        attr_renamed = ar.GetAttribute('attr_renamed')
        self.assertEqual(attr_renamed.Read(), 'bar')

    reopen_after_rename()

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_dim_at_creation(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        '/vsimem/multidim_rename_dim_at_creation_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      dim = rg.CreateDimension('dim', None, None, 2)
      other_dim = rg.CreateDimension('other_dim', None, None, 2)
      var = rg.CreateMDArray(
          'var', [dim, other_dim], gdal.ExtendedDataType.Create(gdal.GDT_Int16)
      )

      with self.assertRaisesRegex(RuntimeError, 'Invalid dimension name'):
        dim.Rename('')

      message = 'A dimension with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        dim.Rename('other_dim')

      self.assertEqual(dim.GetName(), 'dim')
      self.assertEqual(dim.GetFullName(), '/dim')

      dim.Rename('dim_renamed')
      self.assertEqual(dim.GetName(), 'dim_renamed')
      self.assertEqual(dim.GetFullName(), '/dim_renamed')

      self.assertEqual(
          set(x.GetName() for x in rg.GetDimensions()),
          {'dim_renamed', 'other_dim'},
      )

      self.assertEqual(
          [x.GetName() for x in var.GetDimensions()],
          ['dim_renamed', 'other_dim'],
      )

    create()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        self.assertEqual(
            set(x.GetName() for x in rg.GetDimensions()),
            {'dim_renamed', 'other_dim'},
        )

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        rg.GetDimensions()[0].Rename('dim_renamed2')

      self.assertEqual(
          set(x.GetName() for x in rg.GetDimensions()),
          {'dim_renamed', 'other_dim'},
      )

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(
      [('ZARR_V2', 'YES'), ('ZARR_V2', 'NO'), ('ZARR_V3', 'NO')]
  )
  def test_zarr_multidim_rename_dim_after_reopening(
      self, zarr_format: str, create_z_metadata: str
  ):
    filename = pathlib.Path(
        '/vsimem/test_zarr_multidim_rename_dim_after_reopening_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      dim = rg.CreateDimension('dim', None, None, 2)
      other_dim = rg.CreateDimension('other_dim', None, None, 2)
      rg.CreateMDArray(
          'var', [dim, other_dim], gdal.ExtendedDataType.Create(gdal.GDT_Int16)
      )

    create()

    def rename():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()
      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        dim = list(
            filter(lambda dim: dim.GetName() == 'dim', rg.GetDimensions())
        )[0]

      with self.assertRaisesRegex(RuntimeError, 'Invalid dimension name'):
        dim.Rename('')

      message = 'A dimension with same name already exists'
      with self.assertRaisesRegex(RuntimeError, message):
        dim.Rename('other_dim')

      self.assertEqual(dim.GetName(), 'dim')
      self.assertEqual(dim.GetFullName(), '/dim')

      dim.Rename('dim_renamed')
      self.assertEqual(dim.GetName(), 'dim_renamed')
      self.assertEqual(dim.GetFullName(), '/dim_renamed')

      self.assertEqual(
          set(x.GetName() for x in rg.GetDimensions()),
          {
              'dim_renamed',
              'other_dim',
          },
      )

    rename()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()

      with gdal.quiet_warnings():
        # Triggers this for Zarr V3:
        #   Warning 1: fill_value = null is invalid
        self.assertEqual(
            set(x.GetName() for x in rg.GetDimensions()),
            {'dim_renamed', 'other_dim'},
        )

      with self.assertRaisesRegex(
          RuntimeError, 'Dataset not open in update mode'
      ):
        rg.GetDimensions()[0].Rename('dim_renamed2')

      self.assertEqual(
          set(x.GetName() for x in rg.GetDimensions()),
          {'dim_renamed', 'other_dim'},
      )

      var = rg.OpenMDArray('var')
      self.assertEqual(
          [x.GetName() for x in var.GetDimensions()],
          ['dim_renamed', 'other_dim'],
      )

    gdal.RmdirRecursive(filename)

  @parameterized.parameters([
      ('ZARR_V2', 'YES', True),
      ('ZARR_V2', 'NO', True),
      ('ZARR_V3', 'NO', True),
      ('ZARR_V2', 'YES', False),
      ('ZARR_V2', 'NO', False),
      ('ZARR_V3', 'NO', False),
  ])
  def test_zarr_multidim_delete_group_after_reopening(
      self, zarr_format: str, create_z_metadata: str, get_before_delete: bool
  ):
    filename = pathlib.Path(
        '/vsimem/test_zarr_multidim_delete_group_after_reopening_'
        f'{zarr_format}_{create_z_metadata}.zarr'
    )

    def create():
      options = [
          f'FORMAT={zarr_format}',
          f'CREATE_ZMETADATA={create_z_metadata}',
      ]
      ds = self.driver.CreateMultiDimensional(filename, options=options)
      rg = ds.GetRootGroup()
      group = rg.CreateGroup('group')
      group_attr = group.CreateAttribute(
          'group_attr', [], gdal.ExtendedDataType.CreateString()
      )
      group_attr.Write('my_string')
      rg.CreateGroup('other_group')
      dim = group.CreateDimension(
          'dim0', 'unspecified type', 'unspecified direction', 2
      )
      ar = group.CreateMDArray(
          'ar', [dim], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
      )
      attr = ar.CreateAttribute(
          'attr', [], gdal.ExtendedDataType.CreateString()
      )
      attr.Write('foo')
      attr2 = ar.CreateAttribute(
          'attr2', [], gdal.ExtendedDataType.CreateString()
      )
      attr2.Write('foo')

      group.CreateGroup('subgroup')

    create()

    def reopen_readonly():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER)
      rg = ds.GetRootGroup()

      message = 'Dataset not open in update mode'
      with self.assertRaisesRegex(RuntimeError, message):
        rg.DeleteGroup('group')

      self.assertEqual(set(rg.GetGroupNames()), {'group', 'other_group'})

      self.assertIsNotNone(rg.OpenGroup('group'))

    reopen_readonly()

    def delete():
      ds = gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER | gdal.OF_UPDATE)
      rg = ds.GetRootGroup()

      message = 'Group non_existing is not a sub-group of this group'
      with self.assertRaisesRegex(RuntimeError, message):
        rg.DeleteGroup('non_existing')

      if not get_before_delete:
        rg.DeleteGroup('group')

        self.assertEqual(set(rg.GetGroupNames()), {'other_group'})

        with self.assertRaisesRegex(RuntimeError, 'Group group does not exist'):
          rg.OpenGroup('group')

      else:
        group = rg.OpenGroup('group')
        group_attr = group.GetAttribute('group_attr')
        with gdal.quiet_warnings():
          # Triggers this for Zarr V3:
          #   Warning 1: fill_value = null is invalid
          ar = group.OpenMDArray('ar')
        attr = ar.GetAttribute('attr')

        rg.DeleteGroup('group')

        self.assertEqual(set(rg.GetGroupNames()), {'other_group'})

        message = 'Group group does not exist'
        with self.assertRaisesRegex(RuntimeError, message):
          rg.OpenGroup('group')

        message = 'object has been deleted. No action on it is possible'
        with self.assertRaisesRegex(RuntimeError, message):
          group.Rename('renamed')

        with self.assertRaisesRegex(RuntimeError, message):
          group_attr.Rename('renamed')

        with self.assertRaisesRegex(RuntimeError, message):
          ar.GetAttributes()

        with self.assertRaisesRegex(RuntimeError, message):
          attr.Write('foo2')

        with self.assertRaisesRegex(RuntimeError, message):
          ar.GetAttribute('attr2')

    delete()

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()

      self.assertEqual(set(rg.GetGroupNames()), {'other_group'})

    gdal.RmdirRecursive(filename)

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_driver_delete(self, zarr_format: str):
    filename = f'/vsimem/test_zarr_driver_delete_{zarr_format}.zarr'

    self.driver.Create(filename, 1, 1, options=[f'FORMAT={zarr_format}'])

    with gdal.quiet_errors():
      # ZARR_V3 gives: "Warning 1: fill_value = null is invalid" on open.
      self.assertIsNotNone(gdal.Open(filename))

    self.assertEqual(self.driver.Delete(filename), gdal.CE_None)
    self.assertIsNone(gdal.VSIStatL(filename))
    with self.assertRaisesRegex(RuntimeError, 'No such file or directory'):
      gdal.Open(filename)

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_driver_rename(self, zarr_format: str):
    filename = f'/vsimem/test_zarr_driver_rename_{zarr_format}.zarr'
    new_filename = f'/vsimem/test_zarr_driver_rename_{zarr_format}_new.zarr'

    self.driver.Create(filename, 1, 1, options=[f'FORMAT={zarr_format}'])

    with gdal.quiet_errors():
      # ZARR_V3 gives: "Warning 1: fill_value = null is invalid" on open.
      self.assertIsNotNone(gdal.Open(filename))

    self.assertEqual(
        self.driver.Rename(new_filename, filename),
        gdal.CE_None,
    )

    self.assertIsNone(gdal.VSIStatL(filename))
    with self.assertRaisesRegex(RuntimeError, 'No such file or directory'):
      gdal.Open(filename)

    self.assertIsNotNone(gdal.VSIStatL(new_filename))
    with gdal.quiet_errors():
      # ZARR_V3 gives: "Warning 1: fill_value = null is invalid" on open.
      self.assertIsNotNone(gdal.Open(new_filename))

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_driver_copy_files(self, zarr_format: str):
    filename = f'/vsimem/test_zarr_driver_copy_files_{zarr_format}.zarr'
    new_filename = f'/vsimem/test_zarr_driver_copy_files_{zarr_format}_new.zarr'

    self.driver.Create(filename, 1, 1, options=[f'FORMAT={zarr_format}'])

    self.assertEqual(
        self.driver.CopyFiles(new_filename, filename), gdal.CE_None
    )

    with gdal.quiet_errors():
      # ZARR_V3 gives: "Warning 1: fill_value = null is invalid" on open.
      self.assertTrue(gdal.VSIStatL(filename))
      self.assertIsNotNone(gdal.Open(filename))

      self.assertTrue(gdal.VSIStatL(new_filename))
      self.assertIsNotNone(gdal.Open(new_filename))

    gdal.RmdirRecursive(filename)
    gdal.RmdirRecursive(new_filename)

  def test_zarr_multidim_compute_statistics_update_metadata(self):
    filename = (
        '/vsimem/test_netcdf_multidim_compute_statistics_update_metadata.zarr'
    )

    with self.driver.CreateMultiDimensional(filename) as ds:
      rg = ds.GetRootGroup()
      dim0 = rg.CreateDimension('dim0', None, None, 2)
      ar = rg.CreateMDArray(
          'ar', [dim0], gdal.ExtendedDataType.Create(gdal.GDT_Float32)
      )
      ar.Write(array.array('f', [1.5, 2.5]))
      stats = ar.ComputeStatistics(options=['UPDATE_METADATA=YES'])
      self.assertEqual(stats.min, 1.5)
      self.assertEqual(stats.max, 2.5)

    with gdal.OpenEx(filename, gdal.OF_MULTIDIM_RASTER) as ds:
      rg = ds.GetRootGroup()
      ar = rg.OpenMDArray('ar')
      stats = ar.GetStatistics()
      self.assertEqual(stats.min, 1.5)
      self.assertEqual(stats.max, 2.5)
      attr = ar.GetAttribute('actual_range')
      self.assertEqual(list(attr.Read()), [1.5, 2.5])

    gdal.RmdirRecursive(filename)

  def test_zarr_read_cf1(self):
    with gdal.Open(self.getTestFilePath('byte_cf1.zarr')) as ds:
      self.assertIsNotNone(ds)

      srs = ds.GetSpatialRef()
      proj4_str = '+proj=utm +zone=11 +ellps=clrk66 +units=m +no_defs'
      self.assertEqual(srs.ExportToProj4(), proj4_str)
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [1, 2])

  def test_zarr_read_cf1_zarrv3(self):
    with gdal.Open(self.getTestFilePath('byte_cf1.zr3')) as ds:
      self.assertIsNotNone(ds)
      proj4_str = '+proj=utm +zone=11 +ellps=clrk66 +units=m +no_defs'
      self.assertEqual(ds.GetSpatialRef().ExportToProj4(), proj4_str)

  # TODO: b/321622207 - PROJ 35cbc19dbaee99f4b639e8903709c855e840fca9
  @googletest.skip('PROJ missing EPSG:9707')
  def test_zarr_write_WGS84_and_EGM96_height(self):
    out_dirname = pathlib.Path('/vsimem/test_zarr_write_WGS84_and_EGM96_height')
    gdal.Mkdir(out_dirname, 0o755)

    out_zarr = out_dirname / 'out.zarr'

    with self.driver.Create(out_zarr, 1, 1) as ds:
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(9707)
      ds.SetSpatialRef(srs)
    with gdal.Open(out_zarr) as ds:
      srs = ds.GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '9707')
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [2, 1])

    gdal.RmdirRecursive(out_dirname)

  def test_zarr_write_UTM31N_and_EGM96_height(self):
    out_dirname = pathlib.Path(
        '/vsimem/test_zarr_write_UTM31N_and_EGM96_height'
    )
    gdal.Mkdir(out_dirname, 0o755)

    out_zarr = out_dirname / 'out.zarr'
    with self.driver.Create(out_zarr, 1, 1) as ds:
      srs = osr.SpatialReference()
      srs.SetFromUserInput('EPSG:32631+5773')
      ds.SetSpatialRef(srs)
    with gdal.Open(out_zarr) as ds:
      srs = ds.GetSpatialRef()
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [1, 2])

    gdal.RmdirRecursive(out_dirname)

  def test_zarr_write_partial_blocks_compressed(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/small_world.tif')
    out_dirname = pathlib.Path(
        '/vsimem/test_zarr_write_partial_blocks_compressed'
    )
    gdal.Mkdir(out_dirname, 0o755)
    out_zarr = out_dirname / 'test.zarr'
    options = (
        '-of ZARR -co FORMAT=ZARR_V2 -co BLOCKSIZE=3,50,50 '
        '-co COMPRESS=ZLIB -co INTERLEAVE=BAND'
    )

    with gdal.Open(filepath) as src_ds:
      with gdal.Translate(out_zarr, filepath, options=options) as dst_ds:
        self.assertEqual(dst_ds.ReadRaster(), src_ds.ReadRaster())

    gdal.RmdirRecursive(out_dirname)

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_write_cleanup_create_dir_if_bad_blocksize(self, zarr_format):
    filepath = gdrivers_util.GetTestFilePath('gtiff/byte.tif')

    out_dirname = pathlib.Path('/vsimem') / (
        'test_zarr_write_cleanup_create_dir_if_bad_blocksize_append_subdataset_'
        + zarr_format
    )

    gdal.Mkdir(out_dirname, 0o755)

    out_zarr = out_dirname / 'test.zarr'
    options = f'-of ZARR -co FORMAT={zarr_format} -co BLOCKSIZE=1,20,20'
    message = 'Invalid number of values in BLOCKSIZE'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.Translate(out_zarr, filepath, options=options)

    self.assertFalse(gdal.VSIStatL(out_zarr))

    gdal.RmdirRecursive(out_dirname)

  @parameterized.parameters(['ZARR_V2', 'ZARR_V3'])
  def test_zarr_write_cleanup_create_dir_if_bad_blocksize_append_subdataset(
      self, zarr_format: str
  ):
    filepath_1 = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    filepath_2 = gdrivers_util.GetTestFilePath('gtiff/utm.tif')

    out_dirname = pathlib.Path('/vsimem') / (
        'test_zarr_write_cleanup_create_dir_if_bad_blocksize_append_subdataset_'
        + zarr_format
    )

    gdal.Mkdir(out_dirname, 0o755)

    out_zarr = out_dirname / 'byte.zarr'

    gdal.Translate(out_zarr, filepath_1, format=DRIVER)

    self.assertTrue(gdal.VSIStatL(out_zarr))

    options = (
        '-of ZARR -co APPEND_SUBDATASET=YES '
        f'-co FORMAT={zarr_format} '
        '-co ARRAY_NAME=other -co BLOCKSIZE=1,20,20'
    )

    message = 'Invalid number of values in BLOCKSIZE'
    with self.assertRaisesRegex(RuntimeError, message):
      gdal.Translate(out_zarr, filepath_2, options=options)

    self.assertTrue(gdal.VSIStatL(out_zarr))

    with gdal.Open(out_zarr) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

    gdal.RmdirRecursive(out_dirname)


if __name__ == '__main__':
  googletest.main()
