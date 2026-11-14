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

"""Tests for `gdal vector info`.

Ported from upstream autotest/utilities/test_gdalalg_vector_info.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib
import json

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_info_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["info"]


def _has_spatialite() -> bool:
  if not utilities_util.has_driver("SQLite"):
    return False
  with gdal.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
    with utilities_util.vsimem_dir("vector_info_spatialite_chk") as tmp:
      db_path = f"{tmp}/foo.db"
      ds = gdal.GetDriverByName("SQLite").CreateDataSource(
          db_path, options=["SPATIALITE=YES"]
      )
      if ds is None:
        return False
      sql_lyr = ds.ExecuteSQL("SELECT spatialite_version()")
      has_ver = sql_lyr is not None and sql_lyr.GetNextFeature() is not None
      if sql_lyr is not None:
        ds.ReleaseResultSet(sql_lyr)
      ds = None
      return has_ver


@contextlib.contextmanager
def _expect_warning(match: str = "") -> Iterator[None]:
  events: list[tuple[int, str]] = []

  def _handler(err_class: int, err_no: int, msg: str) -> None:
    del err_no
    events.append((err_class, msg))

  gdal.PushErrorHandler(_handler)
  try:
    yield
  finally:
    gdal.PopErrorHandler()

  if not any(lvl == gdal.CE_Warning and match in msg for lvl, msg in events):
    raise AssertionError(
        f"Expected CE_Warning matching {match!r}, got {events!r}"
    )


class GdalalgVectorInfoTest(gdaltest.TestCase, parameterized.TestCase):

  def test_cli_stdout_text_and_json(self) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")
    out, _, code = utilities_util.run_gdal_cli(["vector", "info", path_shp])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("INFO: Open of"))

    out_json, _, code = utilities_util.run_gdal_cli(
        ["vector", "info", "--format", "json", path_shp]
    )
    self.assertEqual(code, 0)
    parsed = json.loads(out_json)
    self.assertEqual(parsed["layers"][0]["name"], "path")
    self.assertNotIn("features", parsed["layers"][0])

  def test_text_and_json_formats(self) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")

    info = _get_info_alg()
    self.assertTrue(info.ParseRunAndFinalize(["--format=text", path_shp]))
    out_text = info["output-string"]
    self.assertTrue(out_text.startswith("INFO: Open of"))
    self.assertIn("Layer name: path", out_text)
    self.assertIn("Geometry: Line String", out_text)
    self.assertIn("Feature Count: 1", out_text)
    self.assertNotIn("OGRFeature", out_text)

    info_json = _get_info_alg()
    self.assertTrue(info_json.ParseRunAndFinalize([path_shp]))
    parsed = json.loads(info_json["output-string"])
    self.assertEqual(parsed["layers"][0]["name"], "path")
    self.assertNotIn("features", parsed["layers"][0])

  def test_features_and_limit(self) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    info_text = _get_info_alg()
    self.assertTrue(
        info_text.ParseRunAndFinalize(["--format=text", "--features", path_shp])
    )
    out_text = info_text["output-string"]
    self.assertTrue(out_text.startswith("INFO: Open of"))
    self.assertIn("Layer name: path", out_text)
    self.assertIn("Geometry: Line String", out_text)
    self.assertIn("Feature Count: 1", out_text)
    self.assertIn("OGRFeature", out_text)

    info_json = _get_info_alg()
    self.assertTrue(info_json.ParseRunAndFinalize(["--features", path_shp]))
    self.assertIn(
        "features", json.loads(info_json["output-string"])["layers"][0]
    )

    info_limit = _get_info_alg()
    self.assertTrue(info_limit.ParseRunAndFinalize(["--limit=2", poly_shp]))
    parsed_limit = json.loads(info_limit["output-string"])
    self.assertLen(parsed_limit["layers"][0]["features"], 2)

  def test_sql_layer_and_errors(self) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")

    info_sql = _get_info_alg()
    self.assertTrue(
        info_sql.ParseRunAndFinalize(
            ["--sql", "SELECT 1 AS foo FROM path", path_shp]
        )
    )
    parsed_sql = json.loads(info_sql["output-string"])
    self.assertLen(parsed_sql["layers"][0]["fields"], 1)
    self.assertEqual(parsed_sql["layers"][0]["fields"][0]["name"], "foo")

    info_lyr = _get_info_alg()
    self.assertTrue(info_lyr.ParseRunAndFinalize(["-l", "path", path_shp]))
    self.assertEqual(
        json.loads(info_lyr["output-string"])["layers"][0]["name"], "path"
    )

    info_err = _get_info_alg()
    with self.assertRaisesRegex(
        RuntimeError, "Cannot find source layer 'invalid'"
    ):
      info_err.ParseRunAndFinalize(["-l", "invalid", path_shp])

    with self.assertRaisesRegex(RuntimeError, "mutually exclusive"):
      gdal.Run(
          "vector",
          "info",
          dataset=path_shp,
          sql="select * from path",
          where="1=1",
      )

  @parameterized.parameters(("0", 0), ("1", 1))
  def test_where(self, cond: str, feature_count: int) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")
    info = _get_info_alg()
    self.assertTrue(info.ParseRunAndFinalize(["--where", cond, path_shp]))
    parsed = json.loads(info["output-string"])
    self.assertEqual(parsed["layers"][0]["featureCount"], feature_count)

  def test_summary(self) -> None:
    path_shp = utilities_util.get_testdata_path("data/path.shp")

    info = _get_info_alg()
    self.assertTrue(info.ParseRunAndFinalize(["--summary", path_shp]))
    layer0 = json.loads(info["output-string"])["layers"][0]
    self.assertNotIn("features", layer0)
    self.assertNotIn("featureCount", layer0)
    self.assertNotIn("geometryFields", layer0)

    with self.assertRaisesRegex(RuntimeError, "mutually exclusive with"):
      _get_info_alg().ParseRunAndFinalize(["--summary", "--features", path_shp])

    info_full = _get_info_alg()
    self.assertTrue(info_full.ParseRunAndFinalize([path_shp]))
    layer0_full = json.loads(info_full["output-string"])["layers"][0]
    self.assertNotIn("features", layer0_full)
    self.assertIn("geometryFields", layer0_full)

    info_text = _get_info_alg()
    self.assertTrue(
        info_text.ParseRunAndFinalize(["--summary", "--format=text", path_shp])
    )
    self.assertEqual(
        info_text["output-string"],
        f"INFO: Open of `{path_shp}'\n"
        "      using driver `ESRI Shapefile' successful.\n"
        "1: path (Line String)\n",
    )

  def test_summary_multi_geometry(self) -> None:
    if not utilities_util.has_driver("SQLite"):
      self.skipTest("SQLite driver not available")
    if not _has_spatialite():
      self.skipTest("SpatiaLite not available")

    path_shp = utilities_util.get_testdata_path("data/path.shp")
    with utilities_util.vsimem_dir("vector_info_multi_geom") as tmp:
      sqlite_multi = f"{tmp}/multi_geometry.db"
      ds = gdal.VectorTranslate(
          sqlite_multi,
          path_shp,
          format="SQLite",
          SQLStatement=(
              "select geometry as geom, st_centroid(geometry) as center,"
              " geometry as geom3, st_buffer(geometry,1) as geom4 from path"
          ),
          SQLDialect="SQLite",
      )
      ds.ExecuteSQL(
          "UPDATE geometry_columns SET geometry_type = 1"
          " where f_geometry_column = 'center'"
      )
      ds.ExecuteSQL(
          "UPDATE geometry_columns SET geometry_type = 3"
          " where f_geometry_column = 'geom4'"
      )
      ds = None

      info = _get_info_alg()
      self.assertTrue(info.ParseRunAndFinalize(["--summary", sqlite_multi]))
      parsed = json.loads(info["output-string"])
      self.assertEqual(
          parsed["layers"][0]["geometryType"],
          ["Line String", "Point", "Line String", "Polygon"],
      )

      info_text = _get_info_alg()
      self.assertTrue(
          info_text.ParseRunAndFinalize(
              ["--summary", "--format=text", sqlite_multi]
          )
      )
      self.assertIn(
          "(Line String, Point, Line String, Polygon)",
          info_text["output-string"],
      )

  def test_dialect(self) -> None:
    if not utilities_util.has_driver("SQLite"):
      self.skipTest("SQLite driver not available")

    path_shp = utilities_util.get_testdata_path("data/path.shp")
    info = _get_info_alg()
    self.assertTrue(
        info.ParseRunAndFinalize([
            "--features",
            "--dialect",
            "SQLite",
            "--sql",
            "SELECT sqlite_version() AS version",
            path_shp,
        ])
    )
    parsed = json.loads(info["output-string"])
    self.assertTrue(
        parsed["layers"][0]["features"][0]["properties"]["version"].startswith(
            "3."
        )
    )

  def test_update(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_info_update") as tmp:
      out_filename = f"{tmp}/poly.gpkg"
      ds = gdal.VectorTranslate(out_filename, poly_shp)
      ds = None

      with _expect_warning("deprecated"):
        gdal.Run(
            "vector",
            "info",
            update=True,
            dataset=out_filename,
            sql="DELETE FROM poly WHERE EAS_ID=170",
        )

      with gdal.OpenEx(out_filename, gdal.OF_VECTOR) as ds_check:
        self.assertEqual(ds_check.GetLayer(0).GetFeatureCount(), 9)


if __name__ == "__main__":
  absltest.main()
