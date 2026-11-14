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

"""Tests for `gdal convert`.

Ported from upstream autotest/utilities/test_gdalalg_convert.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_convert_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry().InstantiateAlg("convert")


class GdalalgConvertTest(gdaltest.TestCase):

  def test_convert_raster(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    with utilities_util.vsimem_dir("convert_raster") as tmp:
      out_path = f"{tmp}/out.tif"
      convert = _get_convert_alg()
      self.assertTrue(convert.ParseRunAndFinalize([utmsmall, out_path]))

      with gdal.Open(out_path) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)

  def test_convert_vector(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("convert_vector") as tmp:
      out_path = f"{tmp}/out.gpkg"
      convert = _get_convert_alg()
      self.assertTrue(convert.ParseRunAndFinalize([poly_shp, out_path]))

      with gdal.OpenEx(out_path) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_invalid_arg_on_raster(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    convert = _get_convert_alg()
    with self.assertRaisesRegex(RuntimeError, "Option '--invalid' is unknown"):
      convert.ParseRunAndFinalize(["--of=MEM", "--invalid", utmsmall, "out"])

  def test_unrecognized_input(self) -> None:
    with utilities_util.vsimem_dir("convert_unrecognized") as tmp:
      bad_input = f"{tmp}/not_a_dataset.txt"
      with gdal.VSIFile(bad_input, "wb") as f:
        f.write(b"not a valid gdal dataset")
      convert = _get_convert_alg()
      with self.assertRaisesRegex(RuntimeError, "not recognized as being"):
        convert.ParseRunAndFinalize(["--of=MEM", bad_input, "out.tif"])


if __name__ == "__main__":
  absltest.main()
