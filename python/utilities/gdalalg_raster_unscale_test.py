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

"""Tests for `gdal raster unscale`.

Ported from upstream autotest/utilities/test_gdalalg_raster_unscale.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_unscale_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["unscale"]


class GdalalgRasterUnscaleTest(gdaltest.TestCase, parameterized.TestCase):

  def test_no_option(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    src_ds.GetRasterBand(1).Fill(1)
    src_ds.GetRasterBand(1).SetScale(2)
    src_ds.GetRasterBand(1).SetOffset(3)

    alg = _get_unscale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        out_ds.GetRasterBand(1).ComputeRasterMinMax(), (1 * 2 + 3, 1 * 2 + 3)
    )
    self.assertEqual(out_ds.GetRasterBand(1).DataType, gdal.GDT_Float32)

  def test_datatype(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    src_ds.GetRasterBand(1).Fill(1)
    src_ds.GetRasterBand(1).SetScale(2.1)
    src_ds.GetRasterBand(1).SetOffset(3)

    alg = _get_unscale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-data-type"] = "Float64"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        out_ds.GetRasterBand(1).ComputeRasterMinMax(),
        (1 * 2.1 + 3, 1 * 2.1 + 3),
    )
    self.assertEqual(out_ds.GetRasterBand(1).DataType, gdal.GDT_Float64)

  @parameterized.parameters(
      (gdal.GDT_Byte, gdal.GDT_Float32),
      (gdal.GDT_Float32, gdal.GDT_Float32),
      (gdal.GDT_Float64, gdal.GDT_Float64),
      (gdal.GDT_CInt16, gdal.GDT_CFloat32),
      (gdal.GDT_CFloat32, gdal.GDT_CFloat32),
      (gdal.GDT_CFloat64, gdal.GDT_CFloat64),
  )
  def test_auto_datatype(self, input_dt: int, out_dt: int) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, input_dt)
    src_ds.GetRasterBand(1).SetScale(2)
    src_ds.GetRasterBand(1).SetOffset(3)

    alg = _get_unscale_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).DataType, out_dt)


if __name__ == "__main__":
  absltest.main()
