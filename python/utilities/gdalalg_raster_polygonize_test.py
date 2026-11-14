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

"""Tests for `gdal raster polygonize`.

Ported from upstream autotest/utilities/test_gdalalg_raster_polygonize.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_polygonize_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["polygonize"]


class GdalalgRasterPolygonizeTest(gdaltest.TestCase, parameterized.TestCase):

  def test_polygonize_basic(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    last_pct = [0.0]

    def my_progress(
        pct: float, unused_msg: str, unused_user_data: object
    ) -> bool:
      last_pct[0] = pct
      return True

    alg = _get_polygonize_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run(my_progress))
    self.assertEqual(last_pct[0], 1.0)

    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayerByName("polygonize")
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
    self.assertEqual(lyr.GetFeatureCount(), 281)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "DN")
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger
    )
    feat = lyr.GetNextFeature()
    self.assertEqual(feat["DN"], 107)
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "POLYGON ((440720 3751320,440720 3751260,440780 3751260,"
        "440780 3751320,440720 3751320))",
    )

  def test_invalid_driver_and_cannot_guess(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_polygonize_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    with self.assertRaisesRegex(
        RuntimeError,
        "Invalid value for argument 'output-format'. Driver 'invalid' does not"
        " exist",
    ):
      alg["output-format"] = "invalid"

    with utilities_util.vsimem_dir("polygonize_bad_ext") as tmpdir:
      bad_path = f"{tmpdir}/invalid.invalid"
      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = bad_path
      with self.assertRaisesRegex(
          RuntimeError, f"Cannot guess driver for {bad_path}"
      ):
        alg.Run()

  def test_cannot_create_dataset_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = os.path.join(tmpdir, "i_do", "not", "exist.gpkg")
      with self.assertRaisesRegex(RuntimeError, "unable to open database file"):
        alg.Run()

  def test_cannot_create_layer_shp(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = os.path.join(tmpdir, "i_do", "not", "exist.shp")
      with self.assertRaisesRegex(RuntimeError, "Cannot create layer 'exist'"):
        alg.Run()

  def test_overwrite_append_and_update(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("polygonize_overwrite") as tmpdir:
      out_filename = f"{tmpdir}/out.shp"

      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)
      self.assertTrue(alg.Finalize())
      ds.Close()

      with ogr.Open(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()
      with ogr.Open(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["update"] = True
      with self.assertRaisesRegex(
          RuntimeError,
          "Layer 'out' already exists. Specify the --overwrite-layer option"
          " to overwrite it, or --append to append to it",
      ):
        alg.Run()
      with ogr.Open(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["append"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281 * 2)
      self.assertTrue(alg.Finalize())
      ds.Close()

      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["overwrite-layer"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)
      self.assertTrue(alg.Finalize())
      ds.Close()

  def test_cannot_find_layer_or_field(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)

    alg = _get_polygonize_alg()
    alg["input"] = byte_tif
    alg["output"] = out_ds
    alg["append"] = True
    with self.assertRaisesRegex(RuntimeError, "Cannot find layer 'polygonize'"):
      alg.Run()

    out_ds.CreateLayer("polygonize")
    alg = _get_polygonize_alg()
    alg["input"] = byte_tif
    alg["output"] = out_ds
    alg["append"] = True
    with self.assertRaisesRegex(
        RuntimeError, "Cannot find field 'DN' in layer 'polygonize'"
    ):
      alg.Run()

  def test_layer_and_field_name(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_polygonize_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "foo"
    alg["attribute-name"] = "bar"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayerByName("foo")
    self.assertEqual(lyr.GetFeatureCount(), 281)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "bar")

  def test_float32_and_int64(self) -> None:
    for rel_path, expected_type in (
        ("../gcore/data/float32.tif", ogr.OFTReal),
        ("../gcore/data/int64.tif", ogr.OFTInteger64),
    ):
      alg = _get_polygonize_alg()
      alg["input"] = utilities_util.get_testdata_path(rel_path)
      alg["output"] = ""
      alg["output-format"] = "MEM"
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayerByName("polygonize")
      self.assertEqual(lyr.GetFeatureCount(), 281)
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "DN")
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(0).GetType(), expected_type
      )

  def test_connect_diagonal_pixels(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_polygonize_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["connect-diagonal-pixels"] = True
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayerByName("polygonize")
    self.assertEqual(lyr.GetFeatureCount(), 229)

  def test_creation_options_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("polygonize_gpkg_co") as tmpdir:
      out_filename = f"{tmpdir}/out.gpkg"
      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
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
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

  @parameterized.parameters(-1, 0, 10)
  def test_commit_interval_gpkg(self, commit_interval: int) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir(
        f"polygonize_commit_{commit_interval}"
    ) as tmpdir:
      out_filename = f"{tmpdir}/out.gpkg"
      alg = _get_polygonize_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["commit-interval"] = commit_interval
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with ogr.Open(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

  def test_creation_options_in_pipeline_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("polygonize_pipeline_co") as tmpdir:
      out_filename = f"{tmpdir}/out.gpkg"
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! polygonize ! write"
              " --creation-option METADATA_TABLES=YES"
              f" --layer-creation-option DESCRIPTION=my_desc {out_filename}"
          ),
      )
      with ogr.Open(out_filename) as ds:
        with ds.ExecuteSQL(
            "SELECT * FROM sqlite_master WHERE name LIKE '%metadata%'"
        ) as sql_lyr:
          self.assertEqual(sql_lyr.GetFeatureCount(), 2)
        self.assertEqual(
            ds.GetLayer(0).GetMetadata_Dict(), {"DESCRIPTION": "my_desc"}
        )
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

  def test_pipeline_with_vector_edit_step(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("polygonize_pipeline_edit") as tmpdir:
      out_filename = f"{tmpdir}/out.shp"
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! polygonize ! edit ! write {out_filename}"
          ),
      )
      with ogr.Open(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)


if __name__ == "__main__":
  absltest.main()
