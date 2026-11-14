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

"""Tests for `gdal raster sieve`.

Ported from upstream autotest/utilities/test_gdalalg_raster_sieve.py.

Upstream copyright:
  Copyright (c) 2025, Alessandro Pasotti
  SPDX-License-Identifier: MIT
"""

import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_SIEVE_SRC_GRD = """\
ncols        5
nrows        7
xllcorner    440720.000000000000
yllcorner    3751020.000000000000
cellsize     60.000000000000
NODATA_value 132
 107 123 132 115 132
 115 132 132 123 123
 115 132 140 132 123
 148 132 123 123 123
 156 156 156 140 132
 156 100 156 101 102
 156 156 156 103 103
"""

_SIEVE_MASK_GRD = """\
ncols        7
nrows        7
xllcorner    440720.000000000000
yllcorner    3750120.000000000000
cellsize     60.000000000000
NODATA_value 0
 0 0 0 0 0 0 0
 0 1 1 1 1 1 1
 0 1 0 0 1 1 1
 0 1 0 2 2 2 1
 0 1 1 2 1 2 1
 0 1 1 2 2 2 1
 0 1 1 1 1 1 1
"""


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["sieve"]


class GdalalgRasterSieveTest(gdaltest.TestCase, parameterized.TestCase):

  def _require_drivers(self) -> None:
    for drv in ("AAIGRID", "GTiff"):
      if not utilities_util.has_driver(drv):
        self.skipTest(f"{drv} driver not available")

  @parameterized.parameters(
      (False, 364, {}),
      (False, 364, {"TILED": "YES"}),
      (False, 364, {"COMPRESS": "LZW"}),
      (True, 370, {}),
      (True, 370, {"TILED": "YES"}),
      (True, 370, {"COMPRESS": "LZW"}),
  )
  def test_sieve(
      self,
      connect_diagonal_pixels: bool,
      expected_checksum: int,
      creation_options: dict[str, str],
  ) -> None:
    self._require_drivers()
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("raster_sieve_basic") as vsimem,
    ):
      result_tif = os.path.join(tmpdir, "test_gdal_sieve.tif")
      src_grd = f"{vsimem}/tmp.grd"
      gdal.FileFromMemBuffer(src_grd, _SIEVE_SRC_GRD)

      alg = _get_alg()
      alg["input"] = src_grd
      alg["output"] = result_tif
      alg["size-threshold"] = 2
      alg["connect-diagonal-pixels"] = connect_diagonal_pixels
      alg["creation-option"] = creation_options
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(result_tif) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.RasterCount, 1)
        dst_band = ds.GetRasterBand(1)

        if creation_options.get("COMPRESS") == "LZW":
          self.assertEqual(
              ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
          )
        if creation_options.get("TILED") == "YES":
          self.assertEqual(dst_band.GetBlockSize(), [256, 256])
        else:
          self.assertNotEqual(dst_band.GetBlockSize(), [256, 256])

        self.assertEqual(dst_band.Checksum(), expected_checksum)

  def test_sieve_mask(self) -> None:
    self._require_drivers()
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("raster_sieve_mask") as vsimem,
    ):
      result_tif = os.path.join(tmpdir, "test_gdal_sieve.tif")
      src_grd = f"{vsimem}/tmp.grd"
      gdal.FileFromMemBuffer(src_grd, _SIEVE_MASK_GRD)

      alg = _get_alg()
      alg["input"] = src_grd
      alg["output"] = result_tif
      alg["size-threshold"] = 2
      alg["mask"] = src_grd

      last_pct = [0.0]

      def on_progress(pct: float, msg: str, user_data: object) -> bool:
        del msg, user_data
        self.assertGreaterEqual(pct, last_pct[0])
        last_pct[0] = pct
        return True

      self.assertTrue(alg.Run(on_progress))
      self.assertTrue(alg.Finalize())
      self.assertEqual(last_pct[0], 1.0)

      with gdal.Open(result_tif) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 42)

      alg_bad = _get_alg()
      alg_bad["input"] = src_grd
      alg_bad["output"] = result_tif
      alg_bad["size-threshold"] = 2
      alg_bad["mask"] = "/i/do_not/exist"
      with self.assertRaises(RuntimeError):
        alg_bad.Run()

  def test_sieve_overwrite(self) -> None:
    self._require_drivers()
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("raster_sieve_overwrite") as vsimem,
    ):
      result_tif = os.path.join(tmpdir, "test_gdal_sieve.tif")
      src_grd = f"{vsimem}/tmp.grd"
      gdal.FileFromMemBuffer(src_grd, _SIEVE_SRC_GRD)

      alg = _get_alg()
      alg["input"] = src_grd
      alg["output"] = result_tif
      alg["size-threshold"] = 2
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

  def test_sieve_cannot_create_temp_file(self) -> None:
    self._require_drivers()
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("raster_sieve_tmp_err") as vsimem,
    ):
      result_tif = os.path.join(tmpdir, "test_gdal_sieve.tif")
      src_grd = f"{vsimem}/tmp.grd"
      gdal.FileFromMemBuffer(src_grd, _SIEVE_SRC_GRD)

      alg = _get_alg()
      alg["input"] = src_grd
      alg["output"] = result_tif
      alg["size-threshold"] = 2
      with gdal.config_options({
          "GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET": "YES",
          "CPL_TMPDIR": "/i_do/not/exist",
      }):
        with self.assertRaises(RuntimeError):
          alg.Run()


if __name__ == "__main__":
  absltest.main()
