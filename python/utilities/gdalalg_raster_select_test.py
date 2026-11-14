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

"""Tests for `gdal raster select`.

Ported from upstream autotest/utilities/test_gdalalg_raster_select.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_select_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["select"]


class GdalalgRasterSelectTest(gdaltest.TestCase):

  def test_select_bands_and_mask(self) -> None:
    rgbsmall_tif = utilities_util.get_testdata_path(
        "../gcore/data/rgbsmall.tif"
    )
    with utilities_util.vsimem_dir("raster_select") as tmp:
      out_filename = f"{tmp}/out.tif"
      alg = _get_select_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(
              ["--band=3,2,1,mask", rgbsmall_tif, out_filename]
          )
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.RasterCount, 4)
        self.assertEqual(
            ds.GetRasterBand(1).GetMaskFlags(),
            gdal.GMF_ALPHA + gdal.GMF_PER_DATASET,
        )
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(4)],
            [21349, 21053, 21212, 30658],
        )

  def test_select_mask_band(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 3, 1)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 3, 1, b"\x00\x7f\xff")

    alg = _get_select_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["band"] = ["1"]
    alg["mask"] = "1"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\x7f\xff")
    self.assertEqual(
        out_ds.GetRasterBand(1).GetMaskFlags(), gdal.GMF_PER_DATASET
    )
    self.assertEqual(
        out_ds.GetRasterBand(1).GetMaskBand().ReadRaster(), b"\x00\x7f\xff"
    )

  def test_errors(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_select_err") as tmp:
      out_filename = f"{tmp}/out.tif"

      alg = _get_select_alg()
      with self.assertRaisesRegex(RuntimeError, "Invalid band specification"):
        alg.ParseRunAndFinalize(["--band=invalid", byte_tif, out_filename])

      alg2 = _get_select_alg()
      with self.assertRaisesRegex(
          RuntimeError, "Invalid mask band specification"
      ):
        alg2.ParseRunAndFinalize(
            ["--band=1", "--mask=invalid", byte_tif, out_filename]
        )


if __name__ == "__main__":
  absltest.main()
