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

"""Tests for `gdal raster viewshed`.

Ported from upstream autotest/utilities/test_gdalalg_raster_viewshed.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib
import os

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_VIEWSHED_NOMINAL_CHECKSUM = 14695


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["viewshed"]


@contextlib.contextmanager
def _viewshed_input() -> Iterator[str]:
  n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
  with gcore_util.TestTemporaryDirectory() as tmpdir:
    fname = os.path.join(tmpdir, "viewshed_in.tif")
    with gdal.Warp(fname, n43_tif, dstSRS="EPSG:32617") as ds:
      ds.GetRasterBand(1).DeleteNoDataValue()
    yield fname


class GdalalgRasterViewshedTest(gdaltest.TestCase):

  def test_viewshed(self) -> None:
    last_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: object) -> bool:
      del msg, user_data
      last_pct[0] = pct
      return True

    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      self.assertTrue(alg.Run(my_progress))
      self.assertEqual(last_pct[0], 1.0)
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), _VIEWSHED_NOMINAL_CHECKSUM
      )

  def test_overwrite_and_creation_option(self) -> None:
    with (
        _viewshed_input() as src_path,
        utilities_util.vsimem_dir("viewshed_overwrite") as tmp,
    ):
      out_filename = f"{tmp}/out.tif"

      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = out_filename
      alg["position"] = [621528, 4817617, 100]
      alg["creation-option"] = {"COMPRESS": "LZW"}
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(out_filename, gdal.GA_Update) as ds:
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )
        ds.GetRasterBand(1).Fill(0)

      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = out_filename
      alg["position"] = [621528, 4817617, 100]
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = out_filename
      alg["position"] = [621528, 4817617, 100]
      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), _VIEWSHED_NOMINAL_CHECKSUM
      )

  def test_target_height(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["target-height"] = 20
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertNotIn(
          ds.GetRasterBand(1).Checksum(), (-1, 0, _VIEWSHED_NOMINAL_CHECKSUM)
      )

  def test_max_distance_and_out_of_range_value(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["max-distance"] = 2000
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      cs = ds.GetRasterBand(1).Checksum()
      self.assertNotIn(cs, (-1, 0, _VIEWSHED_NOMINAL_CHECKSUM))

      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["max-distance"] = 2000
      alg["out-of-range-value"] = 1
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertNotIn(
          ds.GetRasterBand(1).Checksum(),
          (-1, 0, _VIEWSHED_NOMINAL_CHECKSUM, cs),
      )

  def test_curvature_coefficient(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["curvature-coefficient"] = 1
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertNotIn(
          ds.GetRasterBand(1).Checksum(), (-1, 0, _VIEWSHED_NOMINAL_CHECKSUM)
      )

  def test_band(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["band"] = 2
      with self.assertRaisesRegex(
          RuntimeError,
          "Value of 'band' should be greater or equal than 1 and less or equal"
          " than 1",
      ):
        alg.Run()

  def test_visible_value(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["visible-value"] = 1
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertNotIn(
          ds.GetRasterBand(1).Checksum(), (-1, 0, _VIEWSHED_NOMINAL_CHECKSUM)
      )

  def test_invisible_value(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["invisible-value"] = 1
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertNotIn(
          ds.GetRasterBand(1).Checksum(), (-1, 0, _VIEWSHED_NOMINAL_CHECKSUM)
      )

  def test_nodata(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["dst-nodata"] = 0
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(), _VIEWSHED_NOMINAL_CHECKSUM
      )
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 0)

  def test_mode_cumulative(self) -> None:
    with (
        _viewshed_input() as src_path,
        utilities_util.vsimem_dir("viewshed_cumulative") as tmp,
    ):
      tmp_filename = f"{tmp}/out.tif"

      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = tmp_filename
      alg["height"] = 100
      alg["mode"] = "cumulative"
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(tmp_filename) as ds:
        self.assertAlmostEqual(ds.GetRasterBand(1).Checksum(), 36560, delta=1)

      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = tmp_filename
      alg["height"] = 100
      alg["mode"] = "cumulative"
      alg["overwrite"] = True
      alg["observer-spacing"] = 20
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(tmp_filename) as ds:
        self.assertAlmostEqual(ds.GetRasterBand(1).Checksum(), 30941, delta=10)

      with gdal.GetDriverByName("MEM").Create("", 1, 1) as mem_ds:
        alg = _get_alg()
        alg["input"] = mem_ds
        alg["output"] = tmp_filename
        alg["height"] = 100
        alg["mode"] = "cumulative"
        alg["overwrite"] = True
        with self.assertRaisesRegex(
            RuntimeError,
            "In cumulative mode, the input dataset must be opened by name",
        ):
          alg.Run()

  def test_mode_dem(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["mode"] = "DEM"
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 47060)

  def test_mode_ground(self) -> None:
    with _viewshed_input() as src_path:
      alg = _get_alg()
      alg["input"] = src_path
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["position"] = [621528, 4817617, 100]
      alg["mode"] = "ground"
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 8381)


if __name__ == "__main__":
  absltest.main()
