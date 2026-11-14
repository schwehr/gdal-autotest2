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

"""Tests for `gdal vector set-geom-type`.

Ported from upstream autotest/utilities/test_gdalalg_vector_set_geom_type.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
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


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["set-geom-type"]


class GdalalgVectorSetGeomTypeTest(gdaltest.TestCase, parameterized.TestCase):

  def _assert_geometry_near(
      self,
      feat_or_geom: ogr.Feature | ogr.Geometry,
      expected_wkt: str,
      max_error: float = 0.0001,
  ) -> None:
    actual = (
        feat_or_geom.GetGeometryRef()
        if isinstance(feat_or_geom, ogr.Feature)
        else feat_or_geom
    )
    expected = ogr.CreateGeometryFromWkt(expected_wkt)
    self._assert_geom_tree_near(actual, expected, max_error)

  def _assert_geom_tree_near(
      self,
      actual: ogr.Geometry | None,
      expected: ogr.Geometry | None,
      max_error: float,
  ) -> None:
    self.assertIsNotNone(actual)
    self.assertIsNotNone(expected)
    assert actual is not None and expected is not None
    self.assertEqual(actual.GetGeometryName(), expected.GetGeometryName())
    self.assertEqual(actual.Is3D(), expected.Is3D())
    self.assertEqual(actual.IsMeasured(), expected.IsMeasured())
    self.assertEqual(actual.GetGeometryCount(), expected.GetGeometryCount())
    self.assertEqual(actual.GetPointCount(), expected.GetPointCount())
    if expected.GetGeometryCount() > 0:
      for i in range(expected.GetGeometryCount()):
        self._assert_geom_tree_near(
            actual.GetGeometryRef(i), expected.GetGeometryRef(i), max_error
        )
    else:
      for i in range(expected.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(i), expected.GetX(i), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetY(i), expected.GetY(i), delta=max_error
        )
        if expected.Is3D():
          self.assertAlmostEqual(
              actual.GetZ(i), expected.GetZ(i), delta=max_error
          )
        if expected.IsMeasured():
          self.assertAlmostEqual(
              actual.GetM(i), expected.GetM(i), delta=max_error
          )

  def test_geometry_type(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (3 0)"))
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINTZ"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbPoint25D)
    out_feat = out_lyr.GetNextFeature()
    self._assert_geometry_near(out_feat, "POINT Z (3 0 0)")
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCRandomWrite), 0)
    self.assertEqual(out_lyr.GetExtent(), (3, 3, 0, 0))
    feat0 = out_lyr.GetFeature(0)
    self.assertEqual(feat0.GetFID(), 0)
    self.assertIsNone(out_lyr.GetFeature(-1))

  def test_geometry_type_invalid(self) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(
        RuntimeError, "set-geom-type: Invalid geometry type 'invalid'"
    ):
      alg["geometry-type"] = "invalid"

  @parameterized.parameters("multi", "single", "linear", "curve", "dim")
  def test_geometry_type_exclusive(self, other_option: str) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINT"
    alg[other_option] = "XY" if other_option == "dim" else True
    with self.assertRaisesRegex(RuntimeError, "cannot be used with any of"):
      alg.Run()

  def test_failed_conversion_skip_and_no_skip(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    src_ds = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (3 0)"))
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "LINESTRING"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbLineString)
    out_feat = out_lyr.GetNextFeature()
    self._assert_geometry_near(out_feat, "POINT (3 0)")
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)

    src_ds2 = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr2 = src_ds2.CreateLayer("the_layer")
    f1 = ogr.Feature(src_lyr2.GetLayerDefn())
    f1.SetGeometry(ogr.CreateGeometryFromWkt("POINT (3 0)"))
    src_lyr2.CreateFeature(f1)
    f2 = ogr.Feature(src_lyr2.GetLayerDefn())
    f2.SetGeometry(ogr.CreateGeometryFromWkt("LINESTRING Z (3 0 1,4 0 1)"))
    src_lyr2.CreateFeature(f2)

    alg = _get_alg()
    alg["input"] = src_ds2
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "LINESTRING"
    alg["skip"] = True
    self.assertTrue(alg.Run())
    out_ds2 = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds2.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbLineString)
    out_feat2 = out_lyr.GetNextFeature()
    self._assert_geometry_near(out_feat2, "LINESTRING (3 0,4 0)")
    self.assertIsNone(out_lyr.GetNextFeature())
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 0)

  def test_layer_only_and_feature_only(self) -> None:
    def make_src() -> gdal.Dataset:
      ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
      lyr = ds.CreateLayer("the_layer")
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (3 0)"))
      lyr.CreateFeature(feat)
      return ds

    src_ds = make_src()
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINTZ"
    alg["layer-only"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbPoint25D)
    out_feat = out_lyr.GetNextFeature()
    self._assert_geometry_near(out_feat, "POINT (3 0)")
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)

    src_ds2 = make_src()
    alg = _get_alg()
    alg["input"] = src_ds2
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINTZ"
    alg["feature-only"] = True
    self.assertTrue(alg.Run())
    out_ds2 = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds2.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbUnknown)
    out_feat2 = out_lyr.GetNextFeature()
    self._assert_geometry_near(out_feat2, "POINT Z (3 0 0)")
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)

  @parameterized.parameters(
      (("dim", "xy"), "POINT Z (1 2 3)", "POINT (1 2)"),
      (("dim", "xyz"), "POINT (1 2)", "POINT Z (1 2 0)"),
      (("dim", "xym"), "POINT (1 2 3 4)", "POINT M (1 2 4)"),
      (("dim", "xyzm"), "POINT (1 2)", "POINT ZM (1 2 0 0)"),
      (("dim", "xyzm"), "POINT ZM (1 2 3 4)", "POINT ZM (1 2 3 4)"),
      ("multi", "POINT Z (1 2 3)", "MULTIPOINT Z ((1 2 3))"),
      ("single", "MULTIPOINT Z ((1 2 3))", "POINT Z (1 2 3)"),
      ("single", "MULTILINESTRING ((1 2,3 4))", "LINESTRING (1 2,3 4)"),
      (
          "single",
          "MULTIPOLYGON (((0 0,0 1,1 1,0 0)))",
          "POLYGON ((0 0,0 1,1 1,0 0))",
      ),
      ("single", "MULTICURVE ((1 2,3 4))", "COMPOUNDCURVE ((1 2,3 4))"),
      (
          "single",
          "MULTISURFACE (((0 0,0 1,1 1,0 0)))",
          "CURVEPOLYGON ((0 0,0 1,1 1,0 0))",
      ),
      ("curve", "LINESTRING (0 0,1 1)", "COMPOUNDCURVE ((0 0,1 1))"),
  )
  def test_other_modifiers(
      self, modifier: str | tuple[str, str], in_wkt: str, out_wkt: str
  ) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt(in_wkt))
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    if isinstance(modifier, str):
      alg[modifier] = True
    else:
      alg[modifier[0]] = modifier[1]

    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_feat = out_lyr.GetNextFeature()
    self._assert_geometry_near(out_feat, out_wkt)
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)

  def test_linear_modifier(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("CIRCULARSTRING (0 0,1 1,2 0)"))
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["linear"] = True
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_feat = out_lyr.GetNextFeature()
    out_geom = out_feat.GetGeometryRef()
    self.assertEqual(out_geom.GetGeometryName(), "LINESTRING")
    self.assertEqual(out_geom.GetPointCount(), 46)
    self.assertAlmostEqual(out_geom.GetX(0), 0.0)
    self.assertAlmostEqual(out_geom.GetY(0), 0.0)
    self.assertAlmostEqual(out_geom.GetX(45), 2.0)
    self.assertAlmostEqual(out_geom.GetY(45), 0.0)
    # Midpoint on unit semicircle centered at (1, 0) is near (1, 1).
    self.assertAlmostEqual(out_geom.GetY(22), 0.9993908, delta=1e-4)

  def test_active_layer_and_active_geometry(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)

    lyr1 = src_ds.CreateLayer("the_layer", geom_type=ogr.wkbNone)
    lyr1.CreateGeomField(ogr.GeomFieldDefn("a"))
    lyr1.CreateGeomField(ogr.GeomFieldDefn("b"))
    f1 = ogr.Feature(lyr1.GetLayerDefn())
    f1.SetGeomField(0, ogr.CreateGeometryFromWkt("POINT (1 2)"))
    f1.SetGeomField(1, ogr.CreateGeometryFromWkt("POINT (3 4)"))
    lyr1.CreateFeature(f1)

    lyr2 = src_ds.CreateLayer("other_layer", geom_type=ogr.wkbNone)
    lyr2.CreateGeomField(ogr.GeomFieldDefn("a"))
    lyr2.CreateGeomField(ogr.GeomFieldDefn("b"))
    f2 = ogr.Feature(lyr2.GetLayerDefn())
    f2.SetGeomField(0, ogr.CreateGeometryFromWkt("POINT (5 6)"))
    f2.SetGeomField(1, ogr.CreateGeometryFromWkt("POINT (7 8)"))
    lyr2.CreateFeature(f2)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["geometry-type"] = "POINTZ"
    alg["active-layer"] = "the_layer"
    alg["active-geometry"] = "b"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")

    out_lyr1 = out_ds.GetLayer(0)
    self.assertEqual(
        out_lyr1.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbUnknown
    )
    self.assertEqual(
        out_lyr1.GetLayerDefn().GetGeomFieldDefn(1).GetType(), ogr.wkbPoint25D
    )
    out_f1 = out_lyr1.GetNextFeature()
    self._assert_geometry_near(out_f1.GetGeomFieldRef(0), "POINT (1 2)")
    self._assert_geometry_near(out_f1.GetGeomFieldRef(1), "POINT Z (3 4 0)")

    out_lyr2 = out_ds.GetLayer(1)
    self.assertEqual(
        out_lyr2.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbUnknown
    )
    self.assertEqual(
        out_lyr2.GetLayerDefn().GetGeomFieldDefn(1).GetType(), ogr.wkbUnknown
    )
    out_f2 = out_lyr2.GetNextFeature()
    self._assert_geometry_near(out_f2.GetGeomFieldRef(0), "POINT (5 6)")
    self._assert_geometry_near(out_f2.GetGeomFieldRef(1), "POINT (7 8)")

  def test_autocomplete(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "vector", "set-geom-type", "--geometry-type"]
    )
    self.assertEqual(code, 0)
    tokens = out.split(" ")
    for expected in (
        "GEOMETRY",
        "GEOMETRYZ",
        "GEOMETRYM",
        "GEOMETRYZM",
        "POINT",
        "MULTIPOINT",
    ):
      self.assertIn(expected, tokens)

    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "vector",
        "set-geom-type",
        "--geometry-type",
        "GEOMETRYC",
    ])
    self.assertEqual(code, 0)
    self.assertLen(out.split(" "), 4)

  def test_gdalg_streamed(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": (
            f"gdal vector set-geom-type --geometry-type=MULTIPOLYGON {poly_shp}"
            " --output-format=stream foo"
        ),
        "relative_paths_relative_to_this_file": False,
    }
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      gdalg_filename = os.path.join(tmpdir, "tmp.gdalg.json")
      with open(gdalg_filename, "w", encoding="utf-8") as f:
        json.dump(spec, f)
      with gdal.OpenEx(gdalg_filename) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetGeomType(), ogr.wkbMultiPolygon)
        self.assertEqual(lyr.GetFeatureCount(), 10)


if __name__ == "__main__":
  absltest.main()
