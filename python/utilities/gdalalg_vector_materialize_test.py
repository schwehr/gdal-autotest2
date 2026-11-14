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

"""Tests for `materialize` step in `gdal vector pipeline`.

Ported from upstream autotest/utilities/test_gdalalg_vector_materialize.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_cleaned_list(lst: list[str] | None) -> list[str]:
  if not lst:
    return []
  return [elt for elt in lst if elt not in (".", "..")]


class GdalalgVectorMaterializeTest(gdaltest.TestCase):

  def test_temp_output_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    tab_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: object) -> bool:
      del msg, user_data
      if pct < tab_pct[0]:
        return False
      tab_pct[0] = pct
      return True

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            pipeline=(
                f"read {poly_shp} ! materialize ! write --of stream"
                " streamed_dataset"
            ),
            progress=my_progress,
        ) as alg:
          self.assertEqual(tab_pct[0], 1.0)
          with alg.Output() as ds:
            self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_temp_output_shapefile(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            pipeline=(
                f'read {poly_shp} ! materialize --format "ESRI Shapefile" !'
                " write --of stream streamed_dataset"
            ),
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetDriver().GetDescription(), "ESRI Shapefile")
            self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_temp_output_mem(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            pipeline=(
                f"read {poly_shp} ! materialize --output-format=MEM ! write"
                " --of stream streamed_dataset"
            ),
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetDriver().GetDescription(), "MEM")
            self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_temp_output_sqlite_several_geom_fields(self) -> None:
    if not utilities_util.has_driver("SQLite"):
      self.skipTest("SQLite driver not available")

    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbNone)
    lyr.CreateGeomField(ogr.GeomFieldDefn("geom1"))
    lyr.CreateGeomField(ogr.GeomFieldDefn("geom2"))

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            input=src_ds,
            pipeline="read ! materialize ! write --of stream streamed_dataset",
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetDriver().GetDescription(), "SQLite")
            self.assertEqual(
                ds.GetLayer(0).GetLayerDefn().GetGeomFieldCount(), 2
            )

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_temp_output_sqlite_list_field_type(self) -> None:
    if not utilities_util.has_driver("SQLite"):
      self.skipTest("SQLite driver not available")

    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbPoint)
    lyr.CreateField(ogr.FieldDefn("strlist", ogr.OFTStringList))

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            input=src_ds,
            pipeline=(
                "read ! materialize --co SPATIALITE=NO ! write --of stream"
                " streamed_dataset"
            ),
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetDriver().GetDescription(), "SQLite")
            self.assertEqual(
                ds.GetLayer(0).GetLayerDefn().GetFieldDefn(0).GetType(),
                ogr.OFTStringList,
            )

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_temp_output_flatgeobuf(self) -> None:
    if not utilities_util.has_driver("FlatGeoBuf"):
      self.skipTest("FlatGeoBuf driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            pipeline=(
                f"read {poly_shp} ! materialize --format FlatGeoBuf"
                " --layer-creation-option SPATIAL_INDEX=NO ! write --of stream"
                " streamed_dataset"
            ),
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_temp_output_parquet(self) -> None:
    if not utilities_util.has_driver("Parquet"):
      self.skipTest("Parquet driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdal.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "vector",
            "pipeline",
            pipeline=(
                f"read {poly_shp} ! materialize --format Parquet ! write --of"
                " stream streamed_dataset"
            ),
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertEqual(_get_cleaned_list(gdal.ReadDir(tmpdir)), [])

  def test_read_from_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG") or not utilities_util.has_driver(
        "GPKG"
    ):
      self.skipTest("GDALG or GPKG driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gdal.OpenEx(
        json.dumps({
            "type": "gdal_streamed_alg",
            "command_line": f"gdal pipeline read {poly_shp} ! materialize",
        }),
        gdal.OF_VECTOR,
    ) as ds:
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

    with self.assertRaisesRegex(
        RuntimeError,
        "Step 'materialize' not allowed in stream execution, unless the"
        " GDAL_ALGORITHM_ALLOW_WRITES_IN_STREAM configuration option is set",
    ):
      gdal.OpenEx(
          json.dumps({
              "type": "gdal_streamed_alg",
              "command_line": (
                  f"gdal pipeline read {poly_shp} ! materialize"
                  " --output=illegal"
              ),
          }),
          gdal.OF_VECTOR,
      )


if __name__ == "__main__":
  absltest.main()
