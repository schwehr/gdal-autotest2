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

"""Tests for `gdal vector simplify-coverage`.

Ported from upstream
autotest/utilities/test_gdalalg_vector_simplify_coverage.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

import json
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_POLY_PRJ = (
    'PROJCS["OSGB 1936 / British National Grid",GEOGCS["OSGB 1936",'
    'DATUM["D_OSGB_1936",SPHEROID["Airy_1830",6377563.396,299.3249646]],'
    'PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],'
    'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",49],'
    'PARAMETER["central_meridian",-2],PARAMETER["scale_factor",0.9996012717],'
    'PARAMETER["false_easting",400000],PARAMETER["false_northing",-100000],'
    'UNIT["Meter",1]]'
)


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["simplify-coverage"]


def _count_geom_points(geom: ogr.Geometry | None) -> int:
  if geom is None:
    return 0
  count = geom.GetGeometryCount()
  if count:
    return sum(_count_geom_points(geom.GetGeometryRef(i)) for i in range(count))
  return geom.GetPointCount()


def _count_layer_points(layer: ogr.Layer) -> int:
  layer.ResetReading()
  total = 0
  while True:
    feat = layer.GetNextFeature()
    if feat is None:
      break
    total += _count_geom_points(feat.GetGeometryRef())
  return total


class GdalalgVectorSimplifyCoverageTest(
    gdaltest.TestCase, parameterized.TestCase
):

  def setUp(self) -> None:
    super().setUp()
    if (ogr.GetGEOSVersionMajor(), ogr.GetGEOSVersionMinor()) < (3, 12):
      self.skipTest("GEOS >= 3.12 not available")

  def test_simplify_coverage(self) -> None:
    with utilities_util.vsimem_dir("simplify_coverage") as tmp:
      for ext in ("shp", "shx", "dbf"):
        gdal.CopyFile(
            utilities_util.get_testdata_path(f"../ogr/data/poly.{ext}"),
            f"{tmp}/poly.{ext}",
        )
      gdal.FileFromMemBuffer(f"{tmp}/poly.prj", _POLY_PRJ)
      with gdal.OpenEx(f"{tmp}/poly.shp", gdal.OF_VECTOR) as src_ds:
        src_lyr = src_ds.GetLayer(0)
        src_srs = src_lyr.GetSpatialRef()
        self.assertIsNotNone(src_srs)
        alg = _get_alg()
        alg["input"] = src_ds
        alg["output"] = ""
        alg["output-format"] = "stream"
        alg["tolerance"] = 2

        self.assertTrue(alg.Run())

        dst_ds = utilities_util.get_arg_dataset(alg, "output")
        dst_lyr = dst_ds.GetLayer(0)
        dst_srs = dst_lyr.GetSpatialRef()
        self.assertIsNotNone(dst_srs)
        assert dst_srs is not None
        assert src_srs is not None

        self.assertEqual(dst_lyr.GetFeatureCount(), src_lyr.GetFeatureCount())
        self.assertIsSame(dst_srs, src_srs)
        self.assertLess(
            _count_layer_points(dst_lyr), _count_layer_points(src_lyr)
        )
        self.assertTrue(alg.Finalize())

  def test_active_layer(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    with gdal.OpenEx(poly_shp, gdal.OF_VECTOR) as poly:
      src_ds.CopyLayer(
          poly.GetLayer(0), "poly1", options=["ADVERTIZE_UTF8=YES"]
      )
      src_ds.CopyLayer(
          poly.GetLayer(0), "poly2", options=["ADVERTIZE_UTF8=YES"]
      )

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["tolerance"] = 2
    alg["active-layer"] = "bad"

    with self.assertRaisesRegex(RuntimeError, "layer .* was not found"):
      alg.Run()

    alg["active-layer"] = "poly2"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(dst_ds.GetLayerCount(), 2)
    poly1 = dst_ds.GetLayerByName("poly1")
    poly2 = dst_ds.GetLayerByName("poly2")
    self.assertEqual(poly1.GetFeatureCount(), poly2.GetFeatureCount())
    self.assertLess(_count_layer_points(poly2), _count_layer_points(poly1))
    self.assertTrue(poly1.TestCapability(ogr.OLCStringsAsUTF8))
    self.assertTrue(alg.Finalize())

  @parameterized.parameters(
      "POINT (3 8)",
      "TIN (((0 0,0 1,1 1,0 0)))",
      None,
  )
  def test_non_polygonal_inputs(self, wkt: str | None) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_lyr = src_ds.CreateLayer("layer")
    src_lyr.CreateField(ogr.FieldDefn("a"))

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetField("a", 1)
    if wkt is not None:
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(wkt))
    src_lyr.CreateFeature(feat)

    with utilities_util.vsimem_dir("simplify_coverage_nonpoly") as tmp:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = f"{tmp}/out.shp"
      alg["tolerance"] = 2
      with self.assertRaisesRegex(
          RuntimeError, "can only be performed on polygonal"
      ):
        alg.Run()

  def test_no_tolerance(self) -> None:
    alg = _get_alg()
    alg["input"] = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    alg["output"] = ""
    alg["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError, "tolerance. has not been specified"
    ):
      alg.Run()

  @parameterized.parameters(-5.0, float("nan"))
  def test_bad_tolerance(self, tol: float) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, "should be >= 0"):
      alg["tolerance"] = tol

  def test_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    # test_ogrsf is not built in google3; verify GDALG read directly.
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": (
            f"gdal vector simplify-coverage --tolerance 10 {poly_shp}"
            " --output-format=stream dummy_dataset_name"
        ),
        "relative_paths_relative_to_this_file": False,
    }
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      gdalg_path = os.path.join(tmpdir, "tmp.gdalg.json")
      with open(gdalg_path, "w", encoding="utf-8") as f:
        json.dump(spec, f)
      with gdal.OpenEx(gdalg_path, gdal.OF_VECTOR) as ds:
        lyr = ds.GetLayer(0)
        self.assertGreater(lyr.GetFeatureCount(), 0)


if __name__ == "__main__":
  absltest.main()
