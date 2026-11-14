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

"""Tests for `gdal vector clean-coverage`.

Ported from upstream autotest/utilities/test_gdalalg_vector_clean_coverage.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["clean-coverage"]


def _make_circles_ds() -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").CreateVector("")
  lyr = ds.CreateLayer(
      "circles", osr.SpatialReference(epsg=32145), geom_type=ogr.wkbPolygon
  )
  specs = ((5, 5, 5.0), (10, 15, 6.5), (15, 5, 5.1))
  for x, y, radius in specs:
    pt = ogr.Geometry(ogr.wkbPoint)
    pt.AddPoint(x, y)
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(pt.Buffer(radius))
    lyr.CreateFeature(feat)
  return ds


class GdalalgVectorCleanCoverageTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if (ogr.GetGEOSVersionMajor(), ogr.GetGEOSVersionMinor()) < (3, 14):
      self.skipTest("GEOS >= 3.14 not available")

  def _assert_areas_close(
      self, actual: Sequence[float], expected: Sequence[float]
  ) -> None:
    self.assertLen(actual, len(expected))
    for got, exp in zip(actual, expected):
      self.assertAlmostEqual(got, exp, delta=0.01)

  def test_clean_coverage(self) -> None:
    circles = _make_circles_ds()
    src_lyr = circles.GetLayer(0)

    alg = _get_alg()
    alg["input"] = circles
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), src_lyr.GetFeatureCount())
    self.assertIsSame(dst_lyr.GetSpatialRef(), src_lyr.GetSpatialRef())

    areas = [f.GetGeometryRef().GetArea() for f in dst_lyr]
    self._assert_areas_close(areas, [77.85, 132.67, 80.82])
    self.assertTrue(alg.Finalize())

  @parameterized.parameters(
      (None, [77.85, 132.67, 80.82]),
      ("longest-border", [77.85, 132.67, 80.82]),
      ("max-area", [77.85, 132.67, 80.82]),
      ("min-area", [78.50, 131.26, 81.58]),
      ("min-index", [78.50, 132.11, 80.73]),
  )
  def test_merge_strategy(
      self, strategy: str | None, expected_areas: Sequence[float]
  ) -> None:
    alg = _get_alg()
    alg["input"] = _make_circles_ds()
    alg["output"] = ""
    alg["output-format"] = "stream"
    if strategy is not None:
      alg["merge-strategy"] = strategy
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    areas = [f.GetGeometryRef().GetArea() for f in dst_lyr]
    self._assert_areas_close(areas, expected_areas)
    self.assertTrue(alg.Finalize())

  def test_maximum_gap_width(self) -> None:
    alg = _get_alg()
    alg["input"] = _make_circles_ds()
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["maximum-gap-width"] = 2
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    areas = [f.GetGeometryRef().GetArea() for f in dst_lyr]
    self._assert_areas_close(areas, [80.80, 132.67, 80.82])

  @parameterized.parameters(-5, float("nan"))
  def test_bad_maximum_gap_width(self, tol: float) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, "should be >= 0"):
      alg["maximum-gap-width"] = tol

  @parameterized.parameters(-5, float("nan"))
  def test_bad_snapping_distance(self, tol: float) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, "should be >= 0"):
      alg["snapping-distance"] = tol

  def test_active_layer(self) -> None:
    circles = _make_circles_ds()
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_ds.CopyLayer(circles.GetLayer(0), "poly1")
    src_ds.CopyLayer(circles.GetLayer(0), "poly2")

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["active-layer"] = "bad"
    with self.assertRaisesRegex(RuntimeError, "layer .* was not found"):
      alg.Run()

    alg["active-layer"] = "poly2"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(dst_ds.GetLayerCount(), 2)
    areas1 = [f.GetGeometryRef().GetArea() for f in dst_ds.GetLayer(0)]
    areas2 = [f.GetGeometryRef().GetArea() for f in dst_ds.GetLayer(1)]
    self.assertGreater(sum(areas1), sum(areas2))
    self.assertTrue(alg.Finalize())

  @parameterized.parameters(
      "POINT (3 8)",
      "TIN (((0 0,0 1,1 1,0 0)))",
      None,
  )
  def test_non_polygonal_inputs(self, geom_wkt: str | None) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_lyr = src_ds.CreateLayer("layer")
    src_lyr.CreateField(ogr.FieldDefn("a"))

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat["a"] = 1
    if geom_wkt is not None:
      feat.SetGeometry(ogr.CreateGeometryFromWkt(geom_wkt))
    src_lyr.CreateFeature(feat)

    with utilities_util.vsimem_dir("clean_coverage_nonpoly") as tmp:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = f"{tmp}/out.shp"
      with self.assertRaisesRegex(
          RuntimeError, "can only be performed on polygonal"
      ):
        alg.Run()

  def test_test_ogrsf(self) -> None:
    self.skipTest("test_ogrsf binary not available in google3")


if __name__ == "__main__":
  absltest.main()
