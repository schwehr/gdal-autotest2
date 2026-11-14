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

"""Tests for `gdal vector check-geometry`.

Ported from upstream autotest/utilities/test_gdalalg_vector_check_geometry.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["check-geometry"]


def _geos_at_least(major: int, minor: int) -> bool:
  return (ogr.GetGEOSVersionMajor(), ogr.GetGEOSVersionMinor()) >= (
      major,
      minor,
  )


def _make_wkt_ds(
    wkts: str | Sequence[str],
    geom_type: int = ogr.wkbUnknown,
    epsg: int | None = None,
) -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").CreateVector("")
  srs = osr.SpatialReference(epsg=epsg) if epsg else None
  if srs is not None:
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
  lyr = ds.CreateLayer("polys", srs=srs, geom_type=geom_type)

  wkt_list = [wkts] if isinstance(wkts, str) else list(wkts)
  for idx, wkt in enumerate(wkt_list, start=1):
    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt(wkt)
    if geom is None:
      raise ValueError(f"Invalid WKT: {wkt}")
    feat.SetGeometry(geom)
    feat.SetFID(idx)
    lyr.CreateFeature(feat)
  return ds


def _make_polys_ds() -> gdal.Dataset:
  return _make_wkt_ds(
      [
          "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
          "POLYGON ((0 0, 10 0, 0 10, 10 10, 0 0))",
          (
              "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0),"
              " (15 15, 15 20, 20 15, 15 15))"
          ),
      ],
      geom_type=ogr.wkbPolygon,
      epsg=32145,
  )


def _make_lines_ds() -> gdal.Dataset:
  return _make_wkt_ds(
      [
          "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
          "LINESTRING (0 0, 10 0, 0 10, 10 10, 0 0)",
      ],
      geom_type=ogr.wkbLineString,
      epsg=32145,
  )


class GdalalgVectorCheckGeometryTest(gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if ogr.GetGEOSVersionMajor() == 0:
      self.skipTest("GEOS not available")

  def test_polygons(self) -> None:
    polys = _make_polys_ds()
    src_lyr = polys.GetLayer(0)

    alg = _get_alg()
    alg["input"] = polys
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetName(), "error_location")

    errors = list(dst_lyr)
    self.assertLen(errors, 2)

    self.assertEqual(errors[0]["error"], "Self-intersection")
    self.assertEqual(errors[0].GetGeometryRef().ExportToWkt(), "POINT (5 5)")
    self.assertEqual(errors[0].GetFID(), 2)

    self.assertEqual(errors[1]["error"], "Hole lies outside shell")
    self.assertEqual(errors[1].GetGeometryRef().ExportToWkt(), "POINT (15 15)")
    self.assertEqual(errors[1].GetFID(), 3)

    self.assertEqual(dst_lyr.GetFeatureCount(), 2)
    self.assertIsSame(dst_lyr.GetSpatialRef(), src_lyr.GetSpatialRef())
    self.assertTrue(alg.Finalize())

  def test_include_valid(self) -> None:
    polys = _make_polys_ds()
    src_lyr = polys.GetLayer(0)

    alg = _get_alg()
    alg["input"] = polys
    alg["include-valid"] = True
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), src_lyr.GetFeatureCount())

    out = list(dst_lyr)
    self.assertIsNone(out[0]["error"])
    self.assertIsNone(out[0].GetGeometryRef())
    self.assertEqual(out[0].GetFID(), 1)
    self.assertTrue(alg.Finalize())

  def test_linestring(self) -> None:
    lines_ds = _make_lines_ds()
    alg = _get_alg()
    alg["input"] = lines_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)

    out = list(dst_lyr)
    self.assertEqual(out[0]["error"], "self-intersection")
    if _geos_at_least(3, 14):
      self.assertEqual(out[0].GetGeometryRef().ExportToWkt(), "POINT (5 5)")
    self.assertEqual(out[0].GetFID(), 2)
    self.assertTrue(alg.Finalize())

  def test_linestring_multiple_self_intersections(self) -> None:
    src_ds = _make_wkt_ds("LINESTRING (2 1, 0 0, 2 2, 1 2, 1 0)")
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)

    out = list(dst_lyr)
    self.assertEqual(out[0]["error"], "self-intersection")
    if _geos_at_least(3, 14):
      expected = ogr.CreateGeometryFromWkt("MULTIPOINT ((1 0.5), (1 1))")
      self.assertTrue(out[0].GetGeometryRef().Equals(expected))
    self.assertEqual(out[0].GetFID(), 1)

  def test_curvepolygon(self) -> None:
    src_ds = _make_wkt_ds(
        "CURVEPOLYGON (COMPOUNDCURVE"
        " (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 1 1.1, 0 0)))"
    )
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)
    out = list(dst_lyr)
    self.assertEqual(out[0].GetFID(), 1)

  def test_compoundcurve(self) -> None:
    src_ds = _make_wkt_ds(
        "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 1 1, 0 0))"
    )
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)
    out = list(dst_lyr)
    self.assertEqual(out[0].GetFID(), 1)
    self.assertEqual(out[0]["error"], "self-intersection")
    if _geos_at_least(3, 14):
      expected = ogr.CreateGeometryFromWkt("POINT (1 1)")
      self.assertLess(out[0].GetGeometryRef().Distance(expected), 1e-3)

  def test_single_point_linestring(self) -> None:
    src_ds = _make_wkt_ds("LINESTRING (3 2)")
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)
    out = list(dst_lyr)
    self.assertEqual(
        out[0]["error"], "point array must contain 0 or >1 elements"
    )

  def test_point(self) -> None:
    src_ds = _make_wkt_ds("POINT (3 8)")
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 0)

  def test_geometry_collection(self) -> None:
    src_ds = _make_wkt_ds(
        "GEOMETRYCOLLECTION ("
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)),"
        "LINESTRING (0 0, 10 0, 0 10, 10 10, 0 0))"
    )
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)
    out = list(dst_lyr)
    self.assertEqual(out[0].GetFID(), 1)
    self.assertEqual(out[0]["error"], "self-intersection")
    if _geos_at_least(3, 14):
      self.assertEqual(out[0].GetGeometryRef().ExportToWkt(), "POINT (5 5)")

  def test_non_closed_polygon_ring(self) -> None:
    src_ds = _make_wkt_ds("POLYGON ((7 3, 10 0, 10 10))")
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    errors = list(dst_lyr)
    self.assertEqual(
        errors[0]["error"].lower(),
        "points of linearring do not form a closed linestring",
    )
    self.assertEqual(errors[0].GetGeometryRef().ExportToWkt(), "POINT (7 3)")
    self.assertTrue(alg.Finalize())

  def test_single_point_polygon(self) -> None:
    src_ds = _make_wkt_ds("POLYGON ((7 3))")
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    errors = list(dst_lyr)
    self.assertEqual(
        errors[0]["error"].lower(), "point array must contain 0 or >1 elements"
    )
    self.assertEqual(errors[0].GetGeometryRef().ExportToWkt(), "POINT (7 3)")
    self.assertTrue(alg.Finalize())

  def test_two_point_polygon(self) -> None:
    src_ds = _make_wkt_ds("POLYGON ((7 3, 7 3))")
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    errors = list(dst_lyr)
    self.assertIn(
        "invalid number of points in linearring", errors[0]["error"].lower()
    )
    self.assertEqual(errors[0].GetGeometryRef().ExportToWkt(), "POINT (7 3)")
    self.assertTrue(alg.Finalize())

  def test_empty_shell_polygon_with_hole(self) -> None:
    ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = ds.CreateLayer("src")
    feat = ogr.Feature(lyr.GetLayerDefn())

    geom = ogr.Geometry(ogr.wkbPolygon)
    shell = ogr.Geometry(ogr.wkbLinearRing)
    hole = ogr.Geometry(ogr.wkbLinearRing)
    hole.AddPoint(5, 5)
    hole.AddPoint(10, 5)
    hole.AddPoint(10, 10)
    hole.AddPoint(5, 5)
    geom.AddGeometry(shell)
    geom.AddGeometry(hole)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    errors = list(dst_lyr)
    self.assertIn(
        "shell is empty but holes are not", errors[0]["error"].lower()
    )
    self.assertTrue(alg.Finalize())

  def test_invalid_layer(self) -> None:
    polys = _make_polys_ds()
    alg = _get_alg()
    alg["input"] = polys
    alg["input-layer"] = "does_not_exist"
    alg["output"] = ""
    alg["output-format"] = "stream"

    with self.assertRaisesRegex(RuntimeError, "Cannot find source layer"):
      alg.Run()

  def test_two_layers(self) -> None:
    lines_ds = _make_lines_ds()
    polys_ds = _make_polys_ds()
    ds = gdal.GetDriverByName("MEM").CreateVector("")
    ds.CopyLayer(lines_ds.GetLayer(0), "lines2")
    ds.CopyLayer(polys_ds.GetLayer(0), "poly2")

    alg = _get_alg()
    alg["input"] = ds
    alg["input-layer"] = "poly2"
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 2)
    self.assertTrue(alg.Finalize())

  def test_invalid_geometry_name(self) -> None:
    polys = _make_polys_ds()
    alg = _get_alg()
    alg["input"] = polys
    alg["geometry-field"] = "does_not_exist"
    alg["output"] = ""
    alg["output-format"] = "stream"

    with self.assertRaisesRegex(
        RuntimeError, "Specified geometry field .* does not exist"
    ):
      alg.Run()

  def test_multiple_geometry_fields(self) -> None:
    ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = ds.CreateLayer("source", geom_type=ogr.wkbPoint)
    lyr.CreateGeomField(ogr.GeomFieldDefn("geom2", ogr.wkbPolygon))

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeomField(0, ogr.CreateGeometryFromWkt("POINT (3 7)"))
    feat.SetGeomField(
        1, ogr.CreateGeometryFromWkt("POLYGON ((0 0, 10 0, 0 10, 10 10, 0 0))")
    )
    lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = ds
    alg["geometry-field"] = "geom2"
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    dst_lyr = dst_ds.GetLayer(0)
    self.assertEqual(dst_lyr.GetFeatureCount(), 1)
    self.assertTrue(alg.Finalize())

  def test_multiple_layers(self) -> None:
    ds = gdal.GetDriverByName("MEM").CreateVector("")
    ds.CreateLayer("source1")
    ds.CreateLayer("source2")

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(dst_ds.GetLayer(0).GetName(), "error_location_source1")
    self.assertEqual(dst_ds.GetLayer(1).GetName(), "error_location_source2")

  def test_no_geometry_field(self) -> None:
    ds = gdal.GetDriverByName("MEM").CreateVector("")
    ds.CreateLayer("source", geom_type=ogr.wkbNone)

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())
    dst_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(dst_ds.GetLayerCount(), 0)

    alg2 = _get_alg()
    alg2["input"] = ds
    alg2["output"] = ""
    alg2["output-format"] = "stream"
    alg2["input-layer"] = "source"
    with self.assertRaisesRegex(
        RuntimeError, "Specified layer 'source' has no geometry field"
    ):
      alg2.Run()


if __name__ == "__main__":
  absltest.main()
