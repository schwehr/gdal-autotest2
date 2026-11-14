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

"""Tests for `gdal raster fill-nodata`.

Ported from upstream autotest/utilities/test_gdalalg_raster_fill_nodata.py.

Upstream copyright:
  Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
  SPDX-License-Identifier: MIT
"""

import os
import struct
from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["fill-nodata"]


def _read_byte_pixel(ds: gdal.Dataset, x: int, y: int) -> int:
  raw = ds.GetRasterBand(1).ReadRaster(x, y, 1, 1)
  return struct.unpack("B", raw)[0]


class GdalalgRasterFillNodataTest(gdaltest.TestCase, parameterized.TestCase):

  def _run_alg(
      self, alg: gdal.Algorithm, tmpdir: str, vsimem: str
  ) -> gdal.Dataset:
    result_tif = os.path.join(tmpdir, "test_gdal_fill_nodata_1.tif")
    tmp_filename = f"{vsimem}/tmp.tif"
    nodata_byte = utilities_util.get_testdata_path(
        "../gcore/data/nodata_byte.tif"
    )
    with open(nodata_byte, "rb") as src_f:
      gdal.FileFromMemBuffer(tmp_filename, src_f.read())
    alg["input"] = tmp_filename
    alg["output"] = result_tif

    with gdal.Open(tmp_filename) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.RasterCount, 1)
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 0)
      self.assertEqual(_read_byte_pixel(ds, 1, 1), 0)

    tab_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    self.assertTrue(alg.Run(my_progress))
    self.assertTrue(alg.Finalize())
    self.assertEqual(tab_pct[0], 1.0)

    ds = gdal.Open(result_tif)
    self.assertIsNotNone(ds)
    self.assertEqual(ds.RasterCount, 1)
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 0)
    return ds

  def test_cannot_open_file(self) -> None:
    alg = _get_alg()
    alg["input"] = "/i_do/not/exist/in.tif"
    alg["output"] = "/i_do/not/exist/out.tif"
    with self.assertRaisesRegex(
        RuntimeError, r"No such file or directory|does not exist"
    ):
      alg.Run()

  @parameterized.named_parameters(
      ("default", {}),
      ("tiled", {"TILED": "YES"}),
      ("compress_lzw", {"COMPRESS": "LZW"}),
  )
  def test_creation_options(self, creation_option: dict[str, str]) -> None:
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("fill_nodata_co") as vsimem,
    ):
      alg = _get_alg()
      alg["creation-option"] = creation_option
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 125)
        dst_band = ds.GetRasterBand(1)
        if creation_option.get("COMPRESS") == "LZW":
          self.assertEqual(
              ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
          )
        if creation_option.get("TILED") == "YES":
          self.assertEqual(dst_band.GetBlockSize(), [256, 256])
        else:
          self.assertNotEqual(dst_band.GetBlockSize(), [256, 256])

  def test_overwrite(self) -> None:
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("fill_nodata_ow") as vsimem,
    ):
      alg = _get_alg()
      with self._run_alg(alg, tmpdir, vsimem):
        pass

      alg = _get_alg()
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        self._run_alg(alg, tmpdir, vsimem)

      alg["overwrite"] = True
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 125)

  def test_smoothing(self) -> None:
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("fill_nodata_smooth") as vsimem,
    ):
      alg = _get_alg()
      alg["s"] = 2
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 122)

  def test_max_distance(self) -> None:
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("fill_nodata_dist") as vsimem,
    ):
      alg = _get_alg()
      alg["d"] = 1
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 123)

  def test_strategy(self) -> None:
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("fill_nodata_strat") as vsimem,
    ):
      alg = _get_alg()
      alg["strategy"] = "invdist"
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 125)

      alg["strategy"] = "nearest"
      alg["overwrite"] = True
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 123)

  def test_mask(self) -> None:
    nodata_byte = utilities_util.get_testdata_path(
        "../gcore/data/nodata_byte.tif"
    )
    with (
        gcore_util.TestTemporaryDirectory() as tmpdir,
        utilities_util.vsimem_dir("fill_nodata_mask") as vsimem,
    ):
      mask_tif = f"{vsimem}/mask.tif"
      with open(nodata_byte, "rb") as src_f:
        gdal.FileFromMemBuffer(mask_tif, src_f.read())

      alg = _get_alg()
      alg["mask"] = mask_tif

      with gdal.Open(mask_tif, gdal.GA_Update) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.RasterCount, 1)
        ds.GetRasterBand(1).SetNoDataValue(128)
        ds.WriteRaster(0, 0, 1, 1, b"\x00")

      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 0, 0), 120)

      with gdal.Open(mask_tif, gdal.GA_Update) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.RasterCount, 1)
        ds.WriteRaster(0, 0, 1, 1, b"\x01")
        ds.FlushCache()

      alg["overwrite"] = True
      with self._run_alg(alg, tmpdir, vsimem) as ds:
        self.assertEqual(_read_byte_pixel(ds, 1, 1), 125)

  def test_mask_does_not_exist(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("fill_nodata_mask_err") as vsimem:
      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = f"{vsimem}/out.tif"
      alg["mask"] = "/i/do_not/exist"
      with self.assertRaises(RuntimeError):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
