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

"""Tests for `gdal raster tri`.

Ported from upstream autotest/utilities/test_gdalalg_raster_tri.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Mapping
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["tri"]


class GdalalgRasterTriTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      ({}, 51634),
      ({"algorithm": "Riley"}, 51634),
      ({"algorithm": "Wilson"}, 4381),
      ({"no-edges": True}, 41233),
  )
  def test_tri_options(
      self, options: Mapping[str, object], expected_checksum: int
  ) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg = _get_alg()
    alg["input"] = n43_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    for key, value in options.items():
      alg[key] = value
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), expected_checksum)

  def test_band(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    src_ds = gdal.Translate("", n43_tif, format="MEM", bandList=[1, 1])
    src_ds.GetRasterBand(1).Fill(0)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["band"] = 2
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 51634)

  def test_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    n43_tif = os.path.abspath(
        utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    )
    with utilities_util.vsimem_dir("raster_tri_gdalg") as tmp:
      out_filename = f"{tmp}/tmp.gdalg.json"
      alg = _get_alg()
      alg["input"] = n43_tif
      alg["output"] = out_filename
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 51634)

  def test_vrt_output_rejected(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    expected_msg = (
        r"tri: VRT output is not supported\. Consider using the GDALG driver"
        r" instead \(files with \.gdalg\.json extension\)"
    )

    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, expected_msg):
      alg["output-format"] = "VRT"

    alg = _get_alg()
    alg["input"] = n43_tif
    alg["output"] = "i_do/not/exist/out.vrt"
    with self.assertRaisesRegex(RuntimeError, expected_msg):
      alg.Run()

    pipeline_alg = gdal.GetGlobalAlgorithmRegistry()["raster"]["pipeline"]
    pipeline_alg["pipeline"] = (
        f"read {n43_tif} ! tri ! write i_do/not/exist/out.foo"
        " --output-format=vrt"
    )
    with self.assertRaisesRegex(RuntimeError, expected_msg):
      pipeline_alg.Run()

    pipeline_alg = gdal.GetGlobalAlgorithmRegistry()["raster"]["pipeline"]
    pipeline_alg["pipeline"] = (
        f"read {n43_tif} ! tri ! write i_do/not/exist/out.vrt"
    )
    with self.assertRaisesRegex(RuntimeError, expected_msg):
      pipeline_alg.Run()


if __name__ == "__main__":
  absltest.main()
