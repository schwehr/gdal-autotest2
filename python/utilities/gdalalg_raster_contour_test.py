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

"""Tests for `gdal raster contour`.

Ported from upstream autotest/utilities/test_gdalalg_raster_contour.py.

Upstream copyright:
  Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_DEM_ASC = """ncols        2
nrows        2
xllcorner    0
yllcorner    0
cellsize     1
4 15
25 36"""


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_contour_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  raster = reg.InstantiateAlg("raster")
  return raster.InstantiateSubAlgorithm("contour")


class GdalalgRasterContourTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      (
          [
              "--interval",
              "10",
              "--min-name",
              "ELEV_MIN",
              "--max-name",
              "ELEV_MAX",
          ],
          True,
          [(4, 10), (10, 20), (20, 30), (30, 36)],
      ),
      (
          ["--interval", "10", "--elevation-name", "ELEV"],
          False,
          [10.0, 20.0, 30.0],
      ),
      (
          ["--interval", "10", "--elevation-name", "ELEV", "--offset", "5"],
          False,
          [5.0, 15.0, 25.0, 35.0],
      ),
      (
          ["--interval", "10", "--elevation-name", "ELEV", "--src-nodata", "4"],
          False,
          [20.0, 30.0],
      ),
      (
          ["--levels", "10", "--levels", "20", "--elevation-name", "ELEV"],
          False,
          [10.0, 20.0],
      ),
      (
          ["--levels", "10,20", "--elevation-name", "ELEV"],
          False,
          [10.0, 20.0],
      ),
      (
          [
              "--levels",
              "10",
              "--min-name",
              "ELEV_MIN",
              "--max-name",
              "ELEV_MAX",
          ],
          True,
          [(10, 20)],
      ),
      (
          [
              "--exp-base",
              "10",
              "--min-name",
              "ELEV_MIN",
              "--max-name",
              "ELEV_MAX",
          ],
          True,
          [(4, 10), (10, 36)],
      ),
      (
          [
              "--levels",
              "MIN,15,MAX",
              "--min-name",
              "ELEV_MIN",
              "--max-name",
              "ELEV_MAX",
          ],
          True,
          [(4, 15), (15, 36)],
      ),
      (
          [
              "--exp-base",
              "10",
              "--levels",
              "10",
              "--min-name",
              "ELEV_MIN",
              "--max-name",
              "ELEV_MAX",
              "--group-transactions",
              "1",
          ],
          True,
          "contour: Argument 'exp-base' is mutually exclusive with 'levels'.",
      ),
      (
          [
              "--interval",
              "10",
              "--levels",
              "10",
              "--min-name",
              "ELEV_MIN",
              "--max-name",
              "ELEV_MAX",
              "--group-transactions",
              "1",
          ],
          True,
          "contour: Argument 'levels' is mutually exclusive with 'interval'.",
      ),
      (
          [],
          True,
          "contour: One of 'interval', 'levels', 'exp-base' must be specified.",
      ),
      (
          ["--interval", "-10"],
          True,
          "Value of argument 'interval' is -10, but should be > 0",
      ),
  )
  def test_contour(
      self,
      options: Sequence[str],
      polygonize: bool,
      expected_elev_values: Sequence[float | tuple[int, int]] | str,
  ) -> None:
    if not utilities_util.has_driver("AAIGRID"):
      self.skipTest("AAIGRID driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("raster_contour") as tmp:
      tmp_out_filename = f"{tmp}/out.shp"
      tmp_filename = f"{tmp}/tmp.asc"
      gdal.FileFromMemBuffer(tmp_filename, _DEM_ASC.encode("ascii"))

      alg = _get_contour_alg()
      alg_options = [tmp_filename, tmp_out_filename, *options]
      if polygonize:
        alg_options.append("--polygonize")

      if isinstance(expected_elev_values, str):
        with self.assertRaises(RuntimeError):
          alg.ParseRunAndFinalize(alg_options)
        self.assertEqual(gdal.GetLastErrorMsg(), expected_elev_values)
        return

      self.assertTrue(alg.ParseRunAndFinalize(alg_options))
      with gdal.OpenEx(tmp_out_filename) as ds:
        lyr = ds.GetLayer()
        for i, feat in enumerate(lyr):
          geom = feat.GetGeometryRef()
          if polygonize:
            self.assertEqual(geom.GetGeometryType(), ogr.wkbPolygon)
            expected = expected_elev_values[i]
            self.assertIsInstance(expected, tuple)
            assert isinstance(expected, tuple)
            expected_min, expected_max = expected
            self.assertEqual(feat.GetField("ELEV_MIN"), expected_min)
            self.assertEqual(feat.GetField("ELEV_MAX"), expected_max)
          else:
            self.assertEqual(geom.GetGeometryType(), ogr.wkbLineString)
            self.assertEqual(feat.GetField("ELEV"), expected_elev_values[i])
        lyr = None

  def test_overwrite(self) -> None:
    if not utilities_util.has_driver("AAIGRID"):
      self.skipTest("AAIGRID driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("raster_contour_overwrite") as tmp:
      tmp_out_filename = f"{tmp}/out.shp"
      tmp_filename = f"{tmp}/tmp.asc"
      gdal.FileFromMemBuffer(tmp_filename, _DEM_ASC.encode("ascii"))

      alg_options = [
          tmp_filename,
          tmp_out_filename,
          "--interval",
          "10",
          "--min-name",
          "ELEV_MIN",
          "--max-name",
          "ELEV_MAX",
      ]
      self.assertTrue(_get_contour_alg().ParseRunAndFinalize(alg_options))

      with self.assertRaisesRegex(RuntimeError, "already exists"):
        _get_contour_alg().ParseRunAndFinalize(alg_options)

      self.assertTrue(
          _get_contour_alg().ParseRunAndFinalize([*alg_options, "--overwrite"])
      )

  def test_creation_options(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with utilities_util.vsimem_dir("raster_contour_co") as tmp:
      out_filename = f"{tmp}/out.gpkg"
      alg = _get_contour_alg()
      alg["input"] = utilities_util.get_testdata_path("../gcore/data/byte.tif")
      alg["output"] = out_filename
      alg["interval"] = 10
      alg["creation-option"] = {"METADATA_TABLES": "YES"}
      alg["layer-creation-option"] = {"DESCRIPTION": "my_desc"}
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with ogr.Open(out_filename) as ds:
        with ds.ExecuteSQL(
            "SELECT * FROM sqlite_master WHERE name LIKE '%metadata%'"
        ) as sql_lyr:
          self.assertEqual(sql_lyr.GetFeatureCount(), 2)
        self.assertEqual(
            ds.GetLayer(0).GetMetadata_Dict(), {"DESCRIPTION": "my_desc"}
        )


if __name__ == "__main__":
  absltest.main()
