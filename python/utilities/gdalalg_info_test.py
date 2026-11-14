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

"""Tests for `gdal info`.

Ported from upstream autotest/utilities/test_gdalalg_info.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence
import json

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_info_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["info"]


class GdalalgInfoTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters((), ("--checksum",))
  def test_on_raster(self, *extra_args: str) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    info = _get_info_alg()
    self.assertTrue(
        info.ParseRunAndFinalize(["--format=text", utmsmall, *extra_args])
    )
    self.assertTrue(info["output-string"].startswith("Driver: GTiff/GeoTIFF"))

  def test_on_raster_invalid_arg(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    info = _get_info_alg()
    with self.assertRaisesRegex(RuntimeError, "Option '--invalid' is unknown"):
      info.ParseRunAndFinalize(["--format=text", "--invalid", utmsmall])

  @parameterized.parameters(
      ("--format=text", "{path}"),
      ("{path}", "--format=text"),
      ("--format=text", "-i", "{path}"),
      ("--format=text", "--input", "{path}"),
      ("--format=text", "--input={path}"),
      ("--format=text", "--dialect=OGRSQL", "{path}"),
  )
  def test_on_vector(self, *templates: str) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")
    args: Sequence[str] = [t.format(path=path_shp) for t in templates]
    info = _get_info_alg()
    self.assertTrue(info.ParseRunAndFinalize(args))
    self.assertTrue(info["output-string"].startswith("INFO: Open of"))

  def test_on_vector_invalid_arg(self) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")
    info = _get_info_alg()
    with self.assertRaisesRegex(RuntimeError, "Option '--invalid' is unknown"):
      info.ParseRunAndFinalize(["--format=text", "--invalid", path_shp])

  def test_invalid_arg(self) -> None:
    info = _get_info_alg()
    with self.assertRaisesRegex(RuntimeError, "Option '--invalid' is unknown"):
      info.ParseRunAndFinalize(["--invalid"])

  def test_run_cannot_be_called_directly(self) -> None:
    info = _get_info_alg()
    info["input"] = gdal.GetDriverByName("MEM").Create("", 1, 1)
    with self.assertRaisesRegex(
        RuntimeError, "method should not be called directly"
    ):
      info.Run()

  def test_mixed_raster_vector(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    with utilities_util.vsimem_dir("gdalalg_info_mixed") as tmp:
      filename = f"{tmp}/test.gpkg"
      gdal.Translate(filename, utmsmall)
      with gdal.OpenEx(filename, gdal.OF_UPDATE) as ds:
        ds.CreateLayer("vector_layer")

      info = _get_info_alg()
      with self.assertRaisesRegex(
          RuntimeError, "has both raster and vector content"
      ):
        info.ParseRunAndFinalize([filename])

      info_invalid = _get_info_alg()
      with self.assertRaisesRegex(
          RuntimeError, "has both raster and vector content"
      ):
        info_invalid.ParseRunAndFinalize([filename, "--invalid"])

  def test_mixed_run_without_arg(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    info = _get_info_alg()
    info["input"] = utmsmall
    with self.assertRaisesRegex(RuntimeError, "should not be called directly"):
      info.Run()

  def test_netcdf_raster(self) -> None:
    if not utilities_util.has_driver("netCDF"):
      self.skipTest("netCDF driver not available")
    byte_nc = utilities_util.get_testdata_path(
        "../gdrivers/data/netcdf/byte.nc"
    )
    info = _get_info_alg()
    self.assertTrue(info.ParseRunAndFinalize([byte_nc]))
    self.assertIn('"geoTransform"', info["output-string"])

  def test_nonexistent_input_with_format(self) -> None:
    info = _get_info_alg()
    info["input"] = "/i/do/not/exist"
    info["format"] = "text"
    with self.assertRaisesRegex(RuntimeError, "/i/do/not/exist"):
      info.Run()

  def test_command_line_and_help(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    out, _, code = utilities_util.run_gdal_cli(["info", byte_tif])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Driver: GTiff/GeoTIFF"))

    out, _, code = utilities_util.run_gdal_cli(
        ["info", "--of", "json", byte_tif]
    )
    self.assertEqual(code, 0)
    self.assertEqual(json.loads(out)["driverShortName"], "GTiff")

    out, _, code = utilities_util.run_gdal_cli(["info", poly_shp])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("INFO: Open of"))

    out, _, code = utilities_util.run_gdal_cli(
        ["info", "--of", "json", poly_shp]
    )
    self.assertEqual(code, 0)
    self.assertEqual(json.loads(out)["driverShortName"], "ESRI Shapefile")

    out, _, code = utilities_util.run_gdal_cli(["info", "--help"])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: gdal info [OPTIONS] <INPUT>"))


if __name__ == "__main__":
  absltest.main()
