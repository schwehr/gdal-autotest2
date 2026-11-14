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

"""Tests for `gdal raster resize`.

Ported from upstream autotest/utilities/test_gdalalg_raster_resize.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from typing import Any

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_resize_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  raster = reg.InstantiateAlg("raster")
  return raster.InstantiateSubAlgorithm("resize")


class GdalalgRasterResizeTest(gdaltest.TestCase):

  def test_resize_aspect_ratio_width(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_resize") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      alg = _get_resize_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(
              ["--size=10,0", byte_tif, out_filename], my_progress
          )
      )
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.RasterXSize, 10)
        self.assertEqual(ds.RasterYSize, 10)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 1192)

  def test_resize_resampling(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_resize_resamp") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      alg = _get_resize_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(
              ["--size=0,10", "-r", "cubic", byte_tif, out_filename],
              my_progress,
          )
      )
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.RasterXSize, 10)
        self.assertEqual(ds.RasterYSize, 10)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 1059)

  def test_resize_percent(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_resize_pct") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      alg = _get_resize_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(
              ["--size=50 %,25%", "-r", "cubic", byte_tif, out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.RasterXSize, 10)
        self.assertEqual(ds.RasterYSize, 5)

  def test_illegal_size(self) -> None:
    alg = _get_resize_alg()
    for bad_size in ("-1", "1.5", "1.5x", "1.5 x", "2147483648"):
      with self.assertRaisesRegex(RuntimeError, "Invalid size value"):
        alg["size"] = [bad_size, "1"]

  def test_resize_resolution(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_resize_res") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      alg = _get_resize_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(
              ["--resolution=120,30", "-r", "cubic", byte_tif, out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.RasterXSize, 10)
        self.assertEqual(ds.RasterYSize, 40)


if __name__ == "__main__":
  absltest.main()
