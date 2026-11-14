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

"""Tests for `gdal vector explode-collections`.

Ported from upstream
autotest/utilities/test_gdalalg_vector_explode_collections.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["explode-collections"]


def _src_ds_two_fields() -> gdal.Dataset:
  src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
  src_lyr = src_ds.CreateLayer("the_layer", geom_type=ogr.wkbNone)
  src_lyr.CreateGeomField(ogr.GeomFieldDefn("a", ogr.wkbMultiPoint))
  src_lyr.CreateGeomField(ogr.GeomFieldDefn("b", ogr.wkbMultiLineString))

  feat = ogr.Feature(src_lyr.GetLayerDefn())
  feat.SetGeomField(0, ogr.CreateGeometryFromWkt("MULTIPOINT (1 2,3 4)"))
  feat.SetGeomField(
      1, ogr.CreateGeometryFromWkt("MULTILINESTRING ((5 6,7 8),(9 10,11 12))")
  )
  feat.SetFID(1)
  src_lyr.CreateFeature(feat)
  return src_ds


class GdalalgVectorExplodeCollectionsTest(gdaltest.TestCase):

  def _assert_geom_equal(
      self, got: ogr.Geometry | None, expected: ogr.Geometry | str
  ) -> None:
    exp = (
        ogr.CreateGeometryFromWkt(expected)
        if isinstance(expected, str)
        else expected
    )
    if got is None or exp is None:
      self.fail(f"Expected non-null geometry, got={got}, exp={exp}")
    self.assertEqual(got.GetGeometryName(), exp.GetGeometryName())
    self.assertEqual(got.GetGeometryCount(), exp.GetGeometryCount())
    self.assertEqual(got.GetPointCount(), exp.GetPointCount())
    if got.GetGeometryCount() > 0:
      for i in range(got.GetGeometryCount()):
        self._assert_geom_equal(got.GetGeometryRef(i), exp.GetGeometryRef(i))
    else:
      for i in range(got.GetPointCount()):
        self.assertAlmostEqual(got.GetX(i), exp.GetX(i), delta=1e-4)
        self.assertAlmostEqual(got.GetY(i), exp.GetY(i), delta=1e-4)
        self.assertAlmostEqual(got.GetZ(i), exp.GetZ(i), delta=1e-4)

  def test_explode_collections(self) -> None:
    alg = _get_alg()
    alg["input"] = _src_ds_two_fields()
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(
        out_lyr.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbPoint
    )
    self.assertEqual(
        out_lyr.GetLayerDefn().GetGeomFieldDefn(1).GetType(), ogr.wkbLineString
    )

    expected = [
        (1, "POINT (1 2)", "LINESTRING (5 6,7 8)"),
        (2, "POINT (1 2)", "LINESTRING (9 10,11 12)"),
        (3, "POINT (3 4)", "LINESTRING (5 6,7 8)"),
        (4, "POINT (3 4)", "LINESTRING (9 10,11 12)"),
    ]
    for fid, wkt_a, wkt_b in expected:
      out_f = out_lyr.GetNextFeature()
      if out_f is None:
        self.fail(f"Expected feature {fid}")
      self._assert_geom_equal(out_f.GetGeomFieldRef(0), wkt_a)
      self._assert_geom_equal(out_f.GetGeomFieldRef(1), wkt_b)
      self.assertEqual(out_f.GetFID(), fid)

    self.assertEqual(out_lyr.GetFeatureCount(), 4)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCStringsAsUTF8), 0)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
    self.assertEqual(out_lyr.GetExtent(), (1, 3, 2, 4))

    got = []
    out_ds.ResetReading()
    while True:
      f, _ = out_ds.GetNextFeature()
      if not f:
        break
      got.append(str(f))
      self.assertEqual(f.GetFID(), len(got))
    self.assertLen(got, 4)

  def test_active_geometry_a(self) -> None:
    alg = _get_alg()
    alg["input"] = _src_ds_two_fields()
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["active-geometry"] = "a"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(
        out_lyr.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbPoint
    )
    self.assertEqual(
        out_lyr.GetLayerDefn().GetGeomFieldDefn(1).GetType(),
        ogr.wkbMultiLineString,
    )

    for fid, wkt_a in ((1, "POINT (1 2)"), (2, "POINT (3 4)")):
      out_f = out_lyr.GetNextFeature()
      if out_f is None:
        self.fail(f"Expected feature {fid}")
      self._assert_geom_equal(out_f.GetGeomFieldRef(0), wkt_a)
      self._assert_geom_equal(
          out_f.GetGeomFieldRef(1),
          "MULTILINESTRING ((5 6,7 8),(9 10,11 12))",
      )
      self.assertEqual(out_f.GetFID(), fid)

    self.assertEqual(out_lyr.GetFeatureCount(), 2)

  def test_active_geometry_b(self) -> None:
    alg = _get_alg()
    alg["input"] = _src_ds_two_fields()
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["active-geometry"] = "b"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(
        out_lyr.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbMultiPoint
    )
    self.assertEqual(
        out_lyr.GetLayerDefn().GetGeomFieldDefn(1).GetType(), ogr.wkbLineString
    )

    for fid, wkt_b in (
        (1, "LINESTRING (5 6,7 8)"),
        (2, "LINESTRING (9 10,11 12)"),
    ):
      out_f = out_lyr.GetNextFeature()
      if out_f is None:
        self.fail(f"Expected feature {fid}")
      self._assert_geom_equal(out_f.GetGeomFieldRef(0), "MULTIPOINT (1 2,3 4)")
      self._assert_geom_equal(out_f.GetGeomFieldRef(1), wkt_b)
      self.assertEqual(out_f.GetFID(), fid)

    self.assertEqual(out_lyr.GetFeatureCount(), 2)

  def test_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)

    src_lyr = src_ds.CreateLayer("the_layer", geom_type=ogr.wkbMultiPoint)
    f = ogr.Feature(src_lyr.GetLayerDefn())
    f.SetGeomField(0, ogr.CreateGeometryFromWkt("MULTIPOINT (1 2,3 4)"))
    src_lyr.CreateFeature(f)

    other_lyr = src_ds.CreateLayer("other_layer", geom_type=ogr.wkbUnknown)
    f = ogr.Feature(other_lyr.GetLayerDefn())
    f.SetGeomField(0, ogr.CreateGeometryFromWkt("MULTIPOINT (5 6,7 8)"))
    other_lyr.CreateFeature(f)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["active-layer"] = "the_layer"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")

    out_lyr0 = out_ds.GetLayer(0)
    self.assertEqual(
        out_lyr0.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbPoint
    )
    for fid, wkt in ((1, "POINT (1 2)"), (2, "POINT (3 4)")):
      out_f = out_lyr0.GetNextFeature()
      if out_f is None:
        self.fail(f"Expected feature {fid}")
      self._assert_geom_equal(out_f.GetGeomFieldRef(0), wkt)
      self.assertEqual(out_f.GetFID(), fid)

    out_lyr1 = out_ds.GetLayer(1)
    self.assertEqual(
        out_lyr1.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbUnknown
    )
    out_f1 = out_lyr1.GetNextFeature()
    if out_f1 is None:
      self.fail("Expected feature in other_layer")
    self._assert_geom_equal(out_f1.GetGeomFieldRef(0), "MULTIPOINT (5 6,7 8)")
    self.assertEqual(out_f1.GetFID(), 0)

  def test_geometry_type(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    f = ogr.Feature(src_lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("MULTIPOINT (1 2,3 4)"))
    f.SetFID(1)
    src_lyr.CreateFeature(f)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINTZ"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbPoint25D)
    for wkt in ("POINT Z (1 2 0)", "POINT Z (3 4 0)"):
      out_f = out_lyr.GetNextFeature()
      if out_f is None:
        self.fail("Expected exploded POINTZ feature")
      self._assert_geom_equal(out_f.GetGeometryRef(), wkt)

  def test_geometry_type_skip(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")

    f = ogr.Feature(src_lyr.GetLayerDefn())
    src_lyr.CreateFeature(f)

    f = ogr.Feature(src_lyr.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "GEOMETRYCOLLECTION (POINT(1 2),LINESTRING(3 4,5 6),POINT(7 8))"
        )
    )
    src_lyr.CreateFeature(f)

    f = ogr.Feature(src_lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (9 10)"))
    src_lyr.CreateFeature(f)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINT"
    alg["skip-on-type-mismatch"] = True
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbPoint)

    out_f0 = out_lyr.GetNextFeature()
    if out_f0 is None:
      self.fail("Expected null-geom feature")
    self.assertIsNone(out_f0.GetGeometryRef())

    for wkt in ("POINT (1 2)", "POINT (7 8)", "POINT (9 10)"):
      out_f = out_lyr.GetNextFeature()
      if out_f is None:
        self.fail(f"Expected {wkt}")
      self._assert_geom_equal(out_f.GetGeometryRef(), wkt)

  def test_type_invalid(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "INVALID"
    with self.assertRaisesRegex(
        RuntimeError, "explode-collections: Invalid geometry type 'INVALID'"
    ):
      alg.Run()

  def test_type_autocomplete(self) -> None:
    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "vector",
        "explode-collections",
        "--geometry-type",
    ])
    self.assertEqual(code, 0)
    self.assertEqual(
        out.split(" "),
        [
            "GEOMETRY",
            "GEOMETRYZ",
            "GEOMETRYM",
            "GEOMETRYZM",
            "POINT",
            "POINTZ",
            "POINTM",
            "POINTZM",
            "LINESTRING",
            "LINESTRINGZ",
            "LINESTRINGM",
            "LINESTRINGZM",
            "POLYGON",
            "POLYGONZ",
            "POLYGONM",
            "POLYGONZM",
            "CIRCULARSTRING",
            "CIRCULARSTRINGZ",
            "CIRCULARSTRINGM",
            "CIRCULARSTRINGZM",
            "COMPOUNDCURVE",
            "COMPOUNDCURVEZ",
            "COMPOUNDCURVEM",
            "COMPOUNDCURVEZM",
            "CURVEPOLYGON",
            "CURVEPOLYGONZ",
            "CURVEPOLYGONM",
            "CURVEPOLYGONZM",
            "POLYHEDRALSURFACE",
            "POLYHEDRALSURFACEZ",
            "POLYHEDRALSURFACEM",
            "POLYHEDRALSURFACEZM",
            "TIN",
            "TINZ",
            "TINM",
            "TINZM",
        ],
    )

    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "vector",
        "explode-collections",
        "--geometry-type",
        "COMPOUNDCURVE",
    ])
    self.assertEqual(code, 0)
    self.assertLen(out.split(" "), 4)

  def test_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    # test_ogrsf is not built in google3; verify GDALG read directly.
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": (
            f"gdal vector explode-collections {poly_shp}"
            " --output-format=stream foo"
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
