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

"""Tests for `gdal raster set-type`.

Ported from upstream autotest/utilities/test_gdalalg_raster_set_type.py.

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


def _get_set_type_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["set-type"]


class GdalalgRasterSetTypeTest(gdaltest.TestCase):

  def test_set_type_string(self) -> None:
    rgbsmall = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    with utilities_util.vsimem_dir("raster_set_type_str") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      alg = _get_set_type_alg()
      alg["datatype"] = "UInt16"
      alg["input"] = rgbsmall
      alg["output"] = out_filename
      self.assertTrue(alg.Run() and alg.Finalize())

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)

  def test_set_type_gdt(self) -> None:
    rgbsmall = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    with utilities_util.vsimem_dir("raster_set_type_gdt") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      alg = _get_set_type_alg()
      alg["datatype"] = gdal.GDT_UInt16
      alg["input"] = rgbsmall
      alg["output"] = out_filename
      self.assertTrue(alg.Run() and alg.Finalize())

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)


if __name__ == "__main__":
  absltest.main()
