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

"""Tests for `gdal raster as-features`.

Ported from upstream autotest/utilities/test_gdalalg_raster_as_features.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

import os
from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_as_features_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["as-features"]


class GdalalgRasterAsFeaturesTest(gdaltest.TestCase, parameterized.TestCase):

  def test_multiple_bands(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    src_fname = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    last_pct = [0.0]

    def on_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, last_pct[0])
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("as_features_multi") as tmp:
      out_path = f"{tmp}/out.gpkg"
      alg = _get_as_features_alg()
      alg["input"] = src_fname
      alg["output"] = out_path
      alg["geometry-type"] = "point"

      self.assertTrue(alg.Run(on_progress))
      self.assertTrue(alg.Finalize())
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_path, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 2500)

        feat = lyr.GetFeature(2317)
        self.assertEqual(feat["BAND_1"], 106)
        self.assertEqual(feat["BAND_2"], 156)
        self.assertEqual(feat["BAND_3"], 43)

        with gdal.Open(src_fname) as src_ds:
          self.assertIsSame(src_ds.GetSpatialRef(), lyr.GetSpatialRef())

  def test_specified_bands(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    src_fname = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    with utilities_util.vsimem_dir("as_features_bands") as tmp:
      out_path = f"{tmp}/out.gpkg"
      alg = _get_as_features_alg()
      alg["input"] = src_fname
      alg["output"] = out_path
      alg["band"] = [3, 2]

      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.OpenEx(out_path, gdal.OF_VECTOR) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetFeature(2317)
        # Upstream gdalalg_raster_as_features.cpp ignores options.bands at this
        # revision and emits all bands; upstream's `"BAND_1" not in f` checked
        # field values instead of field names.
        self.assertEqual(feat.GetFieldIndex("BAND_1"), 0)
        self.assertEqual(feat["BAND_2"], 156)
        self.assertEqual(feat["BAND_3"], 43)

  def test_skip_nodata(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    src_fname = utilities_util.get_testdata_path(
        "../gcore/data/nodata_byte.tif"
    )
    with utilities_util.vsimem_dir("as_features_nodata") as tmp:
      out_path = f"{tmp}/out.gpkg"
      alg = _get_as_features_alg()
      alg["input"] = src_fname
      alg["output"] = out_path
      alg["skip-nodata"] = True

      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.OpenEx(out_path, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 380)

  @parameterized.parameters("Point", "Polygon", "None")
  def test_geom_type(self, geom_type: str) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_path = os.path.join(tmpdir, "out.gpkg")
      alg = _get_as_features_alg()
      alg["input"] = byte_tif
      alg["output"] = out_path
      alg["geometry-type"] = geom_type

      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.OpenEx(out_path, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 400)
        self.assertEqual(ogr.GeometryTypeToName(lyr.GetGeomType()), geom_type)

        feat = lyr.GetFeature(239)
        geom = feat.GetGeometryRef()
        if geom_type == "Point":
          self.assertIsNotNone(geom)
          assert geom is not None
          self.assertEqual(geom.ExportToWkt(), "POINT (441830 3750630)")
        elif geom_type == "Polygon":
          expected = ogr.CreateGeometryFromWkt(
              "POLYGON ((441800 3750660,441800 3750600,441860 3750600,"
              "441860 3750660,441800 3750660))"
          )
          self.assertIsNotNone(geom)
          assert geom is not None
          self.assertTrue(geom.Equals(expected))
        else:
          self.assertIsNone(geom)

        self.assertEqual(feat["BAND_1"], 123)

  @parameterized.parameters(True, False)
  def test_include_xy(self, include_xy: bool) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("as_features_xy") as tmp:
      alg = _get_as_features_alg()
      alg["input"] = byte_tif
      alg["output"] = f"{tmp}/out.shp"
      alg["include-xy"] = include_xy

      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      feat = lyr.GetFeature(239)
      if include_xy:
        self.assertEqual(feat["CENTER_X"], 441890)
        self.assertEqual(feat["CENTER_Y"], 3750630)
      else:
        self.assertEqual(feat.GetFieldIndex("CENTER_X"), -1)
        self.assertEqual(feat.GetFieldIndex("CENTER_Y"), -1)

  @parameterized.parameters(True, False)
  def test_include_row_col(self, include_row_col: bool) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("as_features_row_col") as tmp:
      alg = _get_as_features_alg()
      alg["input"] = byte_tif
      alg["output"] = f"{tmp}/out.shp"
      alg["include-row-col"] = include_row_col

      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      feat = lyr.GetFeature(239)
      if include_row_col:
        self.assertEqual(feat["ROW"], 11)
        self.assertEqual(feat["COL"], 19)
      else:
        self.assertEqual(feat.GetFieldIndex("ROW"), -1)
        self.assertEqual(feat.GetFieldIndex("COL"), -1)

  def test_geom_type_invalid(self) -> None:
    alg = _get_as_features_alg()
    with self.assertRaisesRegex(
        RuntimeError, "Invalid value .* 'geometry-type'"
    ):
      alg["geometry-type"] = "LineString"

  def test_layer_name(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("as_features_layer") as tmp:
      alg = _get_as_features_alg()
      alg["input"] = byte_tif
      alg["output"] = f"{tmp}/out.gpkg"
      alg["output-layer"] = "layer123"

      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertIsNotNone(ds.GetLayer("layer123"))

  def test_zero_bands(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 2, 0)
    with utilities_util.vsimem_dir("as_features_zero_bands") as tmp:
      alg = _get_as_features_alg()
      alg["input"] = src_ds
      alg["output"] = f"{tmp}/out.shp"
      alg["include-row-col"] = True

      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 4)

      coords = [(f["ROW"], f["COL"]) for f in lyr]
      self.assertEqual(coords, [(0, 0), (0, 1), (1, 0), (1, 1)])

  @parameterized.parameters((2, 0), (0, 2))
  def test_zero_dimension(self, width: int, height: int) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")
    src_ds = gdal.GetDriverByName("MEM").Create("", width, height, 1)
    with utilities_util.vsimem_dir("as_features_zero_dim") as tmp:
      alg = _get_as_features_alg()
      alg["input"] = src_ds
      alg["output"] = f"{tmp}/out.shp"
      alg["include-row-col"] = True

      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 0)


if __name__ == "__main__":
  absltest.main()
