# Copyright 2026 Google LLC
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

"""Tests for `gdal raster scale`.

Ported from upstream autotest/utilities/test_gdalalg_raster_scale.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import struct

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_scale_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  raster = reg.InstantiateAlg("raster")
  return raster.InstantiateSubAlgorithm("scale")


class GdalalgRasterScaleTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      (gdal.GDT_Byte, 0, 255),
      (gdal.GDT_Int8, -128, 127),
      (gdal.GDT_UInt16, 0, 65535),
      (gdal.GDT_Int16, -32768, 32767),
      (gdal.GDT_UInt32, 0, (1 << 32) - 1),
      (gdal.GDT_Int32, -(1 << 31), (1 << 31) - 1),
      (gdal.GDT_UInt64, 0, 1.844674407370955e19),
      (gdal.GDT_Int64, -9.223372036854775e18, 9.223372036854773e18),
  )
  def test_scale_no_option(
      self, dt: int, expected_min: float, expected_max: float
  ) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 2, 1, dt)
    src_ds.GetRasterBand(1).WriteRaster(
        0, 0, 1, 2, b"\x01\x02", buf_type=gdal.GDT_Byte
    )

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        out_ds.GetRasterBand(1).ComputeRasterMinMax(),
        (expected_min, expected_max),
    )

  def test_scale_srcmin_srcmax_only(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 2)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 1, 2, b"\x01\x02")

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-min"] = 0
    alg["src-max"] = 3
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).ComputeRasterMinMax(), (85, 170))

  def test_scale_dstmin_dstmax_only(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 2)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 1, 2, b"\x01\x02")

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["dst-min"] = 0
    alg["dst-max"] = 3
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).ComputeRasterMinMax(), (0, 3))

  @parameterized.parameters(
      ("src-max", "scale: src-min must be specified"),
      ("src-min", "scale: src-max must be specified"),
      ("dst-max", "scale: dst-min must be specified"),
      ("dst-min", "scale: dst-max must be specified"),
  )
  def test_scale_missing_bound(self, option: str, err_msg: str) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    src_ds.GetRasterBand(1).Fill(15)

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg[option] = 0
    with self.assertRaisesRegex(RuntimeError, err_msg):
      alg.Run()

  def test_scale_srcmin_srcmax_dstmin_dstmax(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    src_ds.GetRasterBand(1).Fill(15)
    src_ds.GetRasterBand(2).Fill(10)

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-min"] = 10
    alg["src-max"] = 20
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).ComputeRasterMinMax(), (150, 150))
    self.assertEqual(out_ds.GetRasterBand(2).ComputeRasterMinMax(), (100, 100))

  def test_scale_band(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    src_ds.GetRasterBand(1).Fill(15)
    src_ds.GetRasterBand(2).Fill(10)

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["band"] = 1
    alg["src-min"] = 10
    alg["src-max"] = 20
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).ComputeRasterMinMax(), (150, 150))
    self.assertEqual(out_ds.GetRasterBand(2).ComputeRasterMinMax(), (10, 10))

  def test_scale_exponent(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    src_ds.GetRasterBand(1).Fill(15)
    src_ds.GetRasterBand(2).Fill(14)

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-min"] = 10
    alg["src-max"] = 20
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    alg["exponent"] = 1.5
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).ComputeRasterMinMax(), (135, 135))
    self.assertEqual(out_ds.GetRasterBand(2).ComputeRasterMinMax(), (125, 125))

  def test_scale_band_exponent_datatype(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    src_ds.GetRasterBand(1).Fill(15)
    src_ds.GetRasterBand(2).Fill(14)

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-data-type"] = "UInt16"
    alg["band"] = 2
    alg["src-min"] = 10
    alg["src-max"] = 20
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    alg["exponent"] = 1.5
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
    self.assertEqual(out_ds.GetRasterBand(1).ComputeRasterMinMax(), (15, 15))
    self.assertEqual(out_ds.GetRasterBand(2).ComputeRasterMinMax(), (125, 125))

  def test_scale_clip(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 5)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 1, 5, b"\x00\x01\x02\x03\x04")

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-min"] = 1
    alg["src-max"] = 3
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        struct.unpack("B" * 5, out_ds.GetRasterBand(1).ReadRaster()),
        (100, 100, 150, 200, 200),
    )

  def test_scale_no_clip(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 5)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 1, 5, b"\x00\x01\x02\x03\x04")

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-min"] = 1
    alg["src-max"] = 3
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    alg["no-clip"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        struct.unpack("B" * 5, out_ds.GetRasterBand(1).ReadRaster()),
        (50, 100, 150, 200, 250),
    )

  def test_scale_no_clip_exponent(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 5)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 1, 5, b"\x00\x01\x02\x03\x04")

    alg = _get_scale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-min"] = 1
    alg["src-max"] = 3
    alg["dst-min"] = 100
    alg["dst-max"] = 200
    alg["exponent"] = 1.1
    alg["no-clip"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        struct.unpack("B" * 5, out_ds.GetRasterBand(1).ReadRaster()),
        (0, 100, 147, 200, 255),
    )


if __name__ == "__main__":
  absltest.main()
