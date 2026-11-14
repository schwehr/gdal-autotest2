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

"""Tests for `gdal raster color-map`.

Ported from upstream autotest/utilities/test_gdalalg_raster_color_map.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Mapping

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["color-map"]


class GdalalgRasterColorMapTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      ({}, [55066, 37594, 47768], "MEM"),
      ({}, [55066, 37594, 47768], "VRT"),
      ({"add-alpha": True}, [55066, 37594, 47768, 48613], "MEM"),
      ({"add-alpha": True}, [55066, 37594, 47768, 48613], "VRT"),
      ({"color-selection": "exact"}, [8073, 53707, 59536], "MEM"),
      ({"color-selection": "exact"}, [8073, 53707, 59536], "VRT"),
      ({"color-selection": "nearest"}, [57296, 42926, 47181], "MEM"),
      ({"color-selection": "nearest"}, [57296, 42926, 47181], "VRT"),
  )
  def test_color_map(
      self,
      options: Mapping[str, object],
      checksum: list[int],
      output_format: str,
  ) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    color_file = utilities_util.get_testdata_path("data/color_file.txt")
    alg = _get_alg()
    alg["input"] = n43_tif
    alg["color-map"] = color_file
    alg["output"] = ""
    alg["output-format"] = output_format
    for k, v in options.items():
      alg[k] = v
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        [
            out_ds.GetRasterBand(i + 1).Checksum()
            for i in range(out_ds.RasterCount)
        ],
        checksum,
    )

  def test_wrong_file(self) -> None:
    alg = _get_alg()
    alg["input"] = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg["color-map"] = "i_do_not_exist.txt"
    alg["output"] = ""
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "Cannot find i_do_not_exist.txt"):
      alg.Run()

  def test_missing_color_map(self) -> None:
    alg = _get_alg()
    alg["input"] = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg["output"] = ""
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError,
        "color-map: Input dataset has no color table and 'color-map' option"
        " was not specified.",
    ):
      alg.Run()

  def test_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    color_file = utilities_util.get_testdata_path("data/color_file.txt")
    with utilities_util.vsimem_dir("raster_color_map_gdalg") as tmp:
      out_filename = f"{tmp}/tmp.gdalg.json"
      alg = _get_alg()
      alg["input"] = n43_tif
      alg["output"] = out_filename
      alg["color-map"] = color_file
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_filename) as ds:
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)],
            [55066, 37594, 47768],
        )

  @parameterized.parameters(
      ({}, [4672, 4672, 4672], "MEM"),
      ({}, [4672, 4672, 4672], "VRT"),
      ({"color-selection": "nearest"}, [4672, 4672, 4672], "MEM"),
      ({"color-selection": "nearest"}, [4672, 4672, 4672], "VRT"),
      ({"add-alpha": True}, [4672, 4672, 4672, 4873], "MEM"),
      ({"add-alpha": True}, [4672, 4672, 4672, 4873], "VRT"),
  )
  def test_from_color_table(
      self,
      options: Mapping[str, object],
      checksum: list[int],
      output_format: str,
  ) -> None:
    if not utilities_util.has_driver("BMP"):
      self.skipTest("BMP driver not available")
    bmp_path = utilities_util.get_testdata_path("../gcore/data/8bit_pal.bmp")
    alg = _get_alg()
    alg["input"] = bmp_path
    alg["output"] = ""
    alg["output-format"] = output_format
    for k, v in options.items():
      alg[k] = v
    if "color-selection" in options:
      gdal.ErrorReset()
      with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
        self.assertTrue(alg.Run())
        self.assertNotEqual(gdal.GetLastErrorMsg(), "")
    else:
      self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        [
            out_ds.GetRasterBand(i + 1).Checksum()
            for i in range(out_ds.RasterCount)
        ],
        checksum,
    )


if __name__ == "__main__":
  absltest.main()
