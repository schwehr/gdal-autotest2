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

"""Tests for `gdal raster stack`.

Ported from upstream autotest/utilities/test_gdalalg_raster_stack.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_stack_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["stack"]


class GdalalgRasterStackTest(gdaltest.TestCase):

  def test_at_filename_error(self) -> None:
    stack = _get_stack_alg()
    stack["input"] = ["@i_do_not_exist"]
    stack["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError, "stack: Cannot open i_do_not_exist"
    ):
      stack.Run()

  def test_resolution_common(self) -> None:
    src1_ds = gdal.GetDriverByName("MEM").Create("", 5, 5)
    src1_ds.SetGeoTransform([2, 3, 0, 49, 0, -3])

    src2_ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
    src2_ds.SetGeoTransform([2, 5, 0, 49, 0, -5])

    stack = _get_stack_alg()
    stack["input"] = [src1_ds, src2_ds]
    stack["output-format"] = "stream"
    stack["resolution"] = "common"
    self.assertTrue(stack.Run())

    ds = utilities_util.get_arg_dataset(stack, "output")
    self.assertEqual(ds.RasterCount, 2)
    self.assertEqual(ds.RasterXSize, 15)
    self.assertEqual(ds.RasterYSize, 15)
    gt = ds.GetGeoTransform()
    expected = (2.0, 1.0, 0.0, 49.0, 0.0, -1.0)
    for got_val, exp_val in zip(gt, expected):
      self.assertAlmostEqual(got_val, exp_val)

  def test_absolute_path(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_stack_abs") as tmp:
      src_path = f"{tmp}/byte.tif"
      out_filename = f"{tmp}/out.vrt"
      gdal.Translate(src_path, byte_tif)

      gdal.Run(
          "raster",
          "stack",
          input=src_path,
          output=out_filename,
          absolute_path=True,
      )

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      with gdal.VSIFile(out_filename, "rb") as f:
        content = f.read().decode("utf-8")
      self.assertIn(f'<SourceFilename relativeToVRT="0">{src_path}', content)
      self.assertNotIn('<SourceFilename relativeToVRT="1">byte.tif', content)

  def test_pipeline(self) -> None:
    src1_ds = gdal.GetDriverByName("MEM").Create("", 3, 1, eType=gdal.GDT_Int16)
    src1_ds.SetGeoTransform([0, 1, 0, 1, 0, -1])
    src1_ds.GetRasterBand(1).Fill(1)

    src2_ds = gdal.GetDriverByName("MEM").Create("", 3, 1, eType=gdal.GDT_Int16)
    src2_ds.SetGeoTransform([0, 1, 0, 1, 0, -1])
    src2_ds.GetRasterBand(1).Fill(2)

    with gdal.Run(
        "raster",
        "pipeline",
        pipeline="stack ! write --of=stream streamed_output",
        input=[src1_ds, src2_ds],
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 3)
      self.assertEqual(out_ds.GetRasterBand(2).Checksum(), 6)


if __name__ == "__main__":
  absltest.main()
