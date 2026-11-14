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

"""Tests for `gdal vector check-coverage`.

Ported from upstream autotest/utilities/test_gdalalg_vector_check_coverage.py.

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


def _get_check_coverage_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["check-coverage"]


def _make_wkt_ds(wkts: Sequence[str], epsg: int | None = None) -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").CreateVector("")
  srs = None
  if epsg is not None:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(epsg)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
  lyr = ds.CreateLayer("polys", srs=srs, geom_type=ogr.wkbUnknown)
  for i, wkt in enumerate(wkts):
    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt(wkt)
    assert geom is not None
    feat.SetGeometry(geom)
    feat.SetFID(i + 1)
    lyr.CreateFeature(feat)
  return ds


def _three_rectangles_ds() -> gdal.Dataset:
  return _make_wkt_ds(
      [
          "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
          "POLYGON ((9.9 0, 20 0, 20 10, 9.9 10, 9.9 0))",
          "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))",
      ],
      epsg=32145,
  )


class GdalalgVectorCheckCoverageTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if (ogr.GetGEOSVersionMajor(), ogr.GetGEOSVersionMinor()) < (3, 12):
      self.skipTest("Requires GEOS >= 3.12")

  @parameterized.parameters(True, False)
  def test_check_coverage(self, include_valid: bool) -> None:
    three_rectangles = _three_rectangles_ds()
    alg = _get_check_coverage_alg()
    alg["input"] = three_rectangles
    alg["output"] = ""
    alg["output-format"] = "stream"
    if include_valid:
      alg["include-valid"] = True

    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetName(), "invalid_edge")
    self.assertIsSame(
        dst_lyr.GetSpatialRef(), three_rectangles.GetLayer(0).GetSpatialRef()
    )
    expected_count = 3 if include_valid else 2
    self.assertEqual(dst_lyr.GetFeatureCount(), expected_count)

    features = [f for f in dst_lyr]
    self.assertLen(features, expected_count)
    self.assertFalse(features[0].GetGeometryRef().IsEmpty())
    self.assertFalse(features[1].GetGeometryRef().IsEmpty())
    if include_valid:
      self.assertTrue(features[2].GetGeometryRef().IsEmpty())

    self.assertTrue(alg.Finalize())

  def test_invalid_layer_and_geometry_name(self) -> None:
    three_rectangles = _three_rectangles_ds()

    alg = _get_check_coverage_alg()
    alg["input"] = three_rectangles
    alg["input-layer"] = "does_not_exist"
    alg["output"] = ""
    alg["output-format"] = "stream"
    with self.assertRaisesRegex(RuntimeError, "Cannot find source layer"):
      alg.Run()

    alg2 = _get_check_coverage_alg()
    alg2["input"] = three_rectangles
    alg2["geometry-field"] = "does_not_exist"
    alg2["output"] = ""
    alg2["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError, "Specified geometry field .* does not exist"
    ):
      alg2.Run()

  def test_two_layers(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    poly_ds = gdal.OpenEx(poly_shp, gdal.OF_VECTOR)
    three_rectangles = _three_rectangles_ds()

    ds = gdal.GetDriverByName("MEM").CreateVector("")
    ds.CopyLayer(poly_ds.GetLayer(0), "poly1")
    ds.CopyLayer(three_rectangles.GetLayer(0), "poly2")

    alg = _get_check_coverage_alg()
    alg["input"] = ds
    alg["input-layer"] = "poly2"
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 2)
    for feat in dst_lyr:
      self.assertEqual(
          feat.GetGeometryRef().GetGeometryType(), ogr.wkbMultiLineString
      )
    self.assertTrue(alg.Finalize())

  def test_multiple_geometry_fields(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    poly_ds = gdal.OpenEx(poly_shp, gdal.OF_VECTOR)

    ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = ds.CreateLayer("source", geom_type=ogr.wkbPolygon)
    lyr.CreateGeomField(ogr.GeomFieldDefn("bufgeom", ogr.wkbPolygon))

    for feat in poly_ds.GetLayer(0):
      f_out = ogr.Feature(lyr.GetLayerDefn())
      geom = feat.GetGeometryRef()
      f_out.SetGeomField(0, geom)
      f_out.SetGeomField(1, geom.Buffer(0.1))
      lyr.CreateFeature(f_out)

    alg = _get_check_coverage_alg()
    alg["input"] = ds
    alg["geometry-field"] = "bufgeom"
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 10)
    self.assertTrue(alg.Finalize())

  def test_multiple_layers_and_no_geometry_field(self) -> None:
    ds_multi = gdal.GetDriverByName("MEM").CreateVector("")
    ds_multi.CreateLayer("source1")
    ds_multi.CreateLayer("source2")

    alg = _get_check_coverage_alg()
    alg["input"] = ds_multi
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(dst_ds.GetLayer(0).GetName(), "invalid_edge_source1")
    self.assertEqual(dst_ds.GetLayer(1).GetName(), "invalid_edge_source2")

    ds_nogeom = gdal.GetDriverByName("MEM").CreateVector("")
    ds_nogeom.CreateLayer("source", geom_type=ogr.wkbNone)

    alg2 = _get_check_coverage_alg()
    alg2["input"] = ds_nogeom
    alg2["output"] = ""
    alg2["output-format"] = "stream"
    self.assertTrue(alg2.Run())
    dst_ds2 = utilities_util.get_arg_dataset(alg2, "output")
    self.assertEqual(dst_ds2.GetLayerCount(), 0)

    alg3 = _get_check_coverage_alg()
    alg3["input"] = ds_nogeom
    alg3["output"] = ""
    alg3["output-format"] = "stream"
    alg3["input-layer"] = "source"
    with self.assertRaisesRegex(
        RuntimeError, "Specified layer 'source' has no geometry field"
    ):
      alg3.Run()


if __name__ == "__main__":
  absltest.main()
