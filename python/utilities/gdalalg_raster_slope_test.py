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

"""Tests for `gdal raster slope`.

Ported from upstream autotest/utilities/test_gdalalg_raster_slope.py.

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
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["slope"]


_VRT_UNSUPPORTED_MSG = (
    r"slope: VRT output is not supported\. Consider using the GDALG"
    r" driver instead \(files with \.gdalg\.json extension\)"
)


class GdalalgRasterSlopeTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      ({}, 5604),
      ({"unit": "percent"}, 10827),
      ({"xscale": 111120, "yscale": 111120}, 4589),
      ({"xscale": 90000, "yscale": 111120}, 5232),
      ({"gradient-alg": "ZevenbergenThorne"}, 6378),
      ({"gradient-alg": "ZevenbergenThorne", "no-edges": True}, 65468),
      ({"no-edges": True}, 64725),
  )
  def test_slope_options(
      self, options: Mapping[str, object], checksum: int
  ) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg = _get_alg()
    alg["input"] = n43_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    for k, v in options.items():
      alg[k] = v
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), checksum)

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
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 5604)

  def test_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")

    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    with utilities_util.vsimem_dir("raster_slope_gdalg") as tmp:
      out_filename = f"{tmp}/tmp.gdalg.json"
      alg = _get_alg()
      alg["input"] = n43_tif
      alg["output"] = out_filename
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 5604)

  def test_vrt_output_from_format(self) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, _VRT_UNSUPPORTED_MSG):
      alg["output-format"] = "VRT"

  def test_vrt_output_from_filename(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg = _get_alg()
    alg["input"] = n43_tif
    alg["output"] = "i_do/not/exist/out.vrt"
    with self.assertRaisesRegex(RuntimeError, _VRT_UNSUPPORTED_MSG):
      alg.Run()

  def test_vrt_output_pipeline_from_format(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg = gdal.GetGlobalAlgorithmRegistry()["raster"]["pipeline"]
    alg["pipeline"] = (
        f"read {n43_tif} ! slope ! write i_do/not/exist/out.foo"
        " --output-format=vrt"
    )
    with self.assertRaisesRegex(RuntimeError, _VRT_UNSUPPORTED_MSG):
      alg.Run()

  def test_vrt_output_pipeline_from_filename(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    alg = gdal.GetGlobalAlgorithmRegistry()["raster"]["pipeline"]
    alg["pipeline"] = f"read {n43_tif} ! slope ! write i_do/not/exist/out.vrt"
    with self.assertRaisesRegex(RuntimeError, _VRT_UNSUPPORTED_MSG):
      alg.Run()

  def test_overview(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    src_ds = gdal.Translate(
        "",
        n43_tif,
        format="MEM",
        width=61,
        resampleAlg=gdal.GRIORA_Bilinear,
    )

    with gdal.Run(
        "raster",
        "slope",
        input=src_ds,
        output="",
        output_format="stream",
    ) as alg:
      out_ds = alg.Output()
      cs = out_ds.GetRasterBand(1).Checksum()
      stats = out_ds.GetRasterBand(1).ComputeStatistics(False)

    src_ds = gdal.Translate("", n43_tif, format="MEM")
    src_ds.BuildOverviews("BILINEAR", [2, 4])

    with gdal.Run(
        "raster",
        "slope",
        input=src_ds,
        output="",
        output_format="stream",
    ) as alg:
      out_ds = alg.Output()
    del src_ds

    band = out_ds.GetRasterBand(1)
    self.assertEqual(band.GetOverviewCount(), 2)
    self.assertIsNone(band.GetOverview(-1))
    self.assertIsNone(band.GetOverview(2))
    self.assertEqual(band.GetOverview(0).XSize, 61)
    self.assertEqual(band.GetOverview(0).YSize, 61)
    self.assertEqual(band.GetOverview(1).XSize, 31)
    self.assertEqual(band.GetOverview(1).YSize, 31)
    self.assertEqual(band.GetOverview(0).Checksum(), cs)
    self.assertEqual(band.GetOverview(0).ComputeStatistics(False), stats)


if __name__ == "__main__":
  absltest.main()
