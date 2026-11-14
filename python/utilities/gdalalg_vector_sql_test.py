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

"""Tests for `gdal vector sql`.

Ported from upstream autotest/utilities/test_gdalalg_vector_sql.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_sql_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  vector = reg.InstantiateAlg("vector")
  return vector.InstantiateSubAlgorithm("sql")


@contextlib.contextmanager
def _capture_warnings() -> Iterator[list[tuple[int, str]]]:
  captured: list[tuple[int, str]] = []

  def handler(err_class: int, _err_no: int, msg: str) -> None:
    captured.append((err_class, msg))

  gdal.PushErrorHandler(handler)
  try:
    yield captured
  finally:
    gdal.PopErrorHandler()


class GdalalgVectorSqlTest(gdaltest.TestCase):

  def test_base(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_base") as tmp:
      out_filename = f"{tmp}/out.shp"
      sql_alg = _get_sql_alg()
      self.assertTrue(
          sql_alg.ParseRunAndFinalize(
              [poly_shp, out_filename, "--sql=select * from poly limit 1"]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)
        self.assertIsNone(ds.GetLayer(-1))
        self.assertIsNone(ds.GetLayer(1))

  def test_layer_name(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_layer_name") as tmp:
      out_filename = f"{tmp}/out"
      sql_alg = _get_sql_alg()
      self.assertTrue(
          sql_alg.ParseRunAndFinalize([
              "--output-layer=foo",
              "--format=ESRI Shapefile",
              poly_shp,
              out_filename,
              "--sql=select * from poly limit 1",
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetName(), "foo")

  def test_sql_errors(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_err") as tmp:
      out_filename = f"{tmp}/out.shp"
      with self.assertRaises(RuntimeError):
        _get_sql_alg().ParseRunAndFinalize([poly_shp, out_filename, "error"])

      with self.assertRaises(RuntimeError):
        _get_sql_alg().ParseRunAndFinalize(
            [poly_shp, out_filename, "--sql=select * from poly", "error"]
        )

      with self.assertRaisesRegex(
          RuntimeError,
          "sql: There should be as many layer names in --output-layer as in"
          " --statement",
      ):
        _get_sql_alg().ParseRunAndFinalize([
            "--output-layer=foo,bar",
            poly_shp,
            f"{tmp}/out_dir",
            "--sql=select * from poly limit 1",
        ])

  def test_several_statements_and_layer_names(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_several") as tmp:
      out_dir = f"{tmp}/out"
      self.assertTrue(
          _get_sql_alg().ParseRunAndFinalize([
              poly_shp,
              out_dir,
              "--format=ESRI Shapefile",
              "--sql=select * from poly limit 1",
              "--sql=select * from poly limit 2",
          ])
      )
      with gdal.OpenEx(out_dir) as ds:
        self.assertEqual(ds.GetLayerCount(), 2)
        self.assertIsNone(ds.GetLayer(-1))
        self.assertIsNone(ds.GetLayer(2))
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetDescription(), "poly")
        self.assertEqual(ds.GetLayer(1).GetFeatureCount(), 2)
        self.assertEqual(ds.GetLayer(1).GetDescription(), "poly2")

      out_named_dir = f"{tmp}/out_named"
      self.assertTrue(
          _get_sql_alg().ParseRunAndFinalize([
              "--output-layer",
              "lyr1,lyr2",
              poly_shp,
              out_named_dir,
              "--format=ESRI Shapefile",
              "--sql=select * from poly limit 1",
              "--sql=select * from poly limit 2",
          ])
      )
      with gdal.OpenEx(out_named_dir) as ds:
        self.assertEqual(ds.GetLayerCount(), 2)
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetDescription(), "lyr1")
        self.assertEqual(ds.GetLayer(1).GetFeatureCount(), 2)
        self.assertEqual(ds.GetLayer(1).GetDescription(), "lyr2")

  def test_dialect_sqlite(self) -> None:
    if not utilities_util.has_driver("SQLite"):
      self.skipTest("SQLite driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_sqlite") as tmp:
      out_filename = f"{tmp}/out.shp"
      self.assertTrue(
          _get_sql_alg().ParseRunAndFinalize([
              "--dialect",
              "SQLite",
              poly_shp,
              out_filename,
              "--sql=select *, sqlite_version() as version from poly limit 1",
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_gpkg_no_result_set_and_update(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_gpkg") as tmp:
      gpkg_path = f"{tmp}/poly.gpkg"
      gdal.VectorTranslate(gpkg_path, poly_shp)

      expected_msg = (
          "Execution of the SQL statement 'PRAGMA foo=bar' did not result in a"
          " result layer"
      )
      with self.assertRaisesRegex(RuntimeError, expected_msg):
        gdal.Run(
            "vector",
            "sql",
            input=gpkg_path,
            output=f"{tmp}/out.gpkg",
            sql="PRAGMA foo=bar",
        )

      with self.assertRaisesRegex(RuntimeError, expected_msg):
        gdal.Run(
            "vector",
            "sql",
            input=gpkg_path,
            output=f"{tmp}/out.gpkg",
            sql=["SELECT 1", "PRAGMA foo=bar"],
        )

      with self.assertRaisesRegex(
          RuntimeError, "Perhaps you need to specify the 'update' argument"
      ):
        gdal.Run("vector", "sql", input=gpkg_path, sql="DELETE FROM poly")

      gdal.Run(
          "vector",
          "sql",
          update=True,
          input=gpkg_path,
          sql="DELETE FROM poly WHERE EAS_ID=170",
      )
      with gdal.OpenEx(gpkg_path) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 9)

  def test_update_return_result_set_warning(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_warn") as tmp:
      copy_shp = f"{tmp}/poly.shp"
      gdal.VectorTranslate(copy_shp, poly_shp)
      with _capture_warnings() as warnings:
        gdal.Run("vector", "sql", input=copy_shp, sql="SELECT * FROM poly")
      self.assertTrue(
          any(
              err_cls == gdal.CE_Warning and "returned a result set" in msg
              for err_cls, msg in warnings
          )
      )

  def test_in_pipeline(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_sql_pipeline") as tmp:
      out_filename = f"{tmp}/poly.shp"
      gdal.Run(
          "vector",
          "pipeline",
          pipeline=(
              f'read {poly_shp} | sql "SELECT * FROM poly WHERE eas_id=170"'
              f" ! write {out_filename}"
          ),
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)


if __name__ == "__main__":
  absltest.main()
