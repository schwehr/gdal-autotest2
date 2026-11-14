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

"""Tests for `gdal vector clip`.

Ported from upstream autotest/utilities/test_gdalalg_vector_clip.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib
import json
import os
import subprocess

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_clip_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["clip"]


class GdalalgVectorClipTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if ogr.GetGEOSVersionMajor() <= 0:
      self.skipTest("GEOS not available")

  @contextlib.contextmanager
  def _assert_warning_raised(self, match: str) -> Iterator[None]:
    messages: list[tuple[int, str]] = []

    def handler(lvl: int, unused_no: int, msg: str) -> None:
      messages.append((lvl, msg))

    gdal.PushErrorHandler(handler)
    try:
      yield
    finally:
      gdal.PopErrorHandler()
    self.assertTrue(
        any(lvl == gdal.CE_Warning and match in msg for lvl, msg in messages),
        f"Expected warning matching {match!r}, got {messages!r}",
    )

  def _assert_geom_equal(
      self,
      actual: ogr.Feature | ogr.Geometry,
      expected_wkt: str,
      max_error: float = 0.0001,
  ) -> None:
    geom = (
        actual.GetGeometryRef() if isinstance(actual, ogr.Feature) else actual
    )
    self.assertIsNotNone(geom)
    assert geom is not None
    expected = ogr.CreateGeometryFromWkt(expected_wkt)
    self.assertIsNotNone(expected)
    assert expected is not None

    if (
        not geom.Is3D()
        and not geom.IsMeasured()
        and geom.Within(expected)
        and expected.Within(geom)
    ):
      return
    if self._geoms_points_close(geom, expected, max_error):
      return
    if expected.GetDimension() == 2:
      diff = geom.SymmetricDifference(expected)
      if diff is not None and diff.GetArea() < 1e-4:
        return
    self.fail(f"Expected {expected_wkt}, got {geom.ExportToWkt()}")

  def _geoms_points_close(
      self,
      actual: ogr.Geometry,
      expected: ogr.Geometry,
      max_error: float,
  ) -> bool:
    if (
        actual.GetGeometryName() != expected.GetGeometryName()
        or actual.GetGeometryCount() != expected.GetGeometryCount()
        or actual.GetPointCount() != expected.GetPointCount()
    ):
      return False
    if actual.GetGeometryCount() > 0:
      return all(
          self._geoms_points_close(
              actual.GetGeometryRef(i), expected.GetGeometryRef(i), max_error
          )
          for i in range(actual.GetGeometryCount())
      )
    for i in range(actual.GetPointCount()):
      if (
          abs(actual.GetX(i) - expected.GetX(i)) > max_error
          or abs(actual.GetY(i) - expected.GetY(i)) > max_error
      ):
        return False
    return True

  def _make_mem_polygon_ds(
      self,
      wkts: list[str],
      srs: osr.SpatialReference | None = None,
      field_name: str = "foo",
      field_val: str = "bar",
      geom_type: int = ogr.wkbUnknown,
      ds_name: str = "",
  ) -> gdal.Dataset:
    ds = gdal.GetDriverByName("MEM").Create(ds_name, 0, 0, 0, gdal.GDT_Unknown)
    lyr = ds.CreateLayer("test", srs=srs, geom_type=geom_type)
    lyr.CreateField(ogr.FieldDefn(field_name))
    for wkt in wkts:
      f = ogr.Feature(lyr.GetLayerDefn())
      if wkt:
        f[field_name] = field_val
        f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(f)
    return ds

  def test_general_behavior(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_clip_general") as tmp:
      out_filename = f"{tmp}/out.gpkg"

      clip = _get_clip_alg()
      self.assertTrue(
          clip.ParseRunAndFinalize(
              ["--bbox", "-1e10,-1e10,1e10,1e10", poly_shp, out_filename]
          )
      )

      with gdal.OpenEx(out_filename, gdal.OF_UPDATE) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        for i in range(10):
          ds.GetLayer(0).DeleteFeature(i + 1)

      clip = _get_clip_alg()
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        clip.ParseRunAndFinalize(
            ["--bbox", "-1e10,-1e10,1e10,1e10", poly_shp, out_filename]
        )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

      clip = _get_clip_alg()
      self.assertTrue(
          clip.ParseRunAndFinalize([
              "--bbox",
              "-1e10,-1e10,1e10,1e10",
              "--overwrite",
              poly_shp,
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      clip = _get_clip_alg()
      self.assertTrue(
          clip.ParseRunAndFinalize([
              "--bbox",
              "-1e10,-1e10,1e10,1e10",
              "--append",
              poly_shp,
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 20)

      clip = _get_clip_alg()
      self.assertTrue(
          clip.ParseRunAndFinalize([
              "--bbox",
              "-1e10,-1e10,1e10,1e10",
              "--update",
              "--nln",
              "layer2",
              poly_shp,
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayerByName("poly").GetFeatureCount(), 20)
        self.assertEqual(ds.GetLayerByName("layer2").GetFeatureCount(), 10)

      clip = _get_clip_alg()
      self.assertTrue(
          clip.ParseRunAndFinalize([
              "--bbox",
              "-1e10,-1e10,1e10,1e10",
              "--of",
              "GPKG",
              "--overwrite-layer",
              "--nln",
              "poly",
              poly_shp,
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayerByName("poly").GetFeatureCount(), 10)
        self.assertEqual(ds.GetLayerByName("layer2").GetFeatureCount(), 10)

  def test_bbox(self) -> None:
    src_ds = self._make_mem_polygon_ds([
        "",
        "POLYGON ((0 0,0 -1,-1 -1,-1 0,0 0))",
        "POLYGON ((0 0,0 1,1 1,1 0,0 0))",
    ])

    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--bbox", "0.2,0.3,0.7,0.8", "--of", "MEM", "--output", "mem_ds"]
        )
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

  def test_bbox_srs(self) -> None:
    srs_wgs84 = osr.SpatialReference()
    srs_wgs84.SetFromUserInput("WGS84")
    srs_wgs84.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    src_ds = self._make_mem_polygon_ds(
        ["POLYGON ((0 0,0 1,1 1,1 0,0 0))"], srs=srs_wgs84
    )

    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments([
            "--bbox",
            (
                "22263.8981586547,33395.9998333802,"
                "77923.6435552915,89058.4864167096"
            ),
            "--bbox-crs",
            "EPSG:3857",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetSpatialRef().GetAuthorityCode(None), "4326")
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

  def test_split_and_dont_split_multipart(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbPolygon)
    src_lyr.CreateField(ogr.FieldDefn("foo"))

    f = ogr.Feature(src_lyr.GetLayerDefn())
    f["foo"] = "bar"
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "POLYGON ((0 0,0 1,1 1,1 0,0.8 0,0.8 0.8,0.2 0.8,0.2 0,0 0))"
        )
    )
    src_lyr.CreateFeature(f)

    f = ogr.Feature(src_lyr.GetLayerDefn())
    f["foo"] = "baz"
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "POLYGON ((0.4 0.4,0.4 0.6,0.6 0.6,0.6 0.4,0.4 0.4))"
        )
    )
    src_lyr.CreateFeature(f)

    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--bbox", "-0.1,0.3,1.1,0.7", "--of", "MEM", "--output", "mem_ds"]
        )
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    ref1_geom = ogr.CreateGeometryFromWkt(
        "POLYGON ((0.0 0.7,0.2 0.7,0.2 0.3,0.0 0.3,0.0 0.7))"
    )
    ref2_geom = ogr.CreateGeometryFromWkt(
        "POLYGON ((1.0 0.3,0.8 0.3,0.8 0.7,1.0 0.7,1.0 0.3))"
    )

    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    g = out_f.GetGeometryRef()
    was_one = g.Within(ref1_geom) and ref1_geom.Within(g)
    self.assertTrue(was_one or (g.Within(ref2_geom) and ref2_geom.Within(g)))

    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    g = out_f.GetGeometryRef()
    if was_one:
      self.assertTrue(g.Within(ref2_geom) and ref2_geom.Within(g))
    else:
      self.assertTrue(g.Within(ref1_geom) and ref1_geom.Within(g))

    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "baz")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.4 0.4,0.4 0.6,0.6 0.6,0.6 0.4,0.4 0.4))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    # Input layer misrepresents its geometry type.
    src_multi = gdal.GetDriverByName("MEM").CreateVector("")
    lyr_multi = src_multi.CreateLayer("test", geom_type=ogr.wkbPolygon)
    f = ogr.Feature(lyr_multi.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt("MULTIPOINT ((0 0), (5 5))").Buffer(2)
    )
    lyr_multi.CreateFeature(f)

    clip = _get_clip_alg()
    clip["input"] = src_multi
    clip["bbox"] = [0, 0, 5, 5]
    clip["output"] = ""
    clip["output-format"] = "MEM"
    self.assertTrue(clip.Run())

    out_ds = clip.Output()
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    out_f = out_lyr.GetNextFeature()
    self.assertIsNotNone(out_f)
    out_geom = out_f.GetGeometryRef()
    self.assertIsNotNone(out_geom)
    self.assertEqual(out_geom.GetGeometryCount(), 2)

  @parameterized.parameters(
      "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))",
      '{"type":"Polygon","coordinates":'
      "[[[0.2,0.8],[0.7,0.8],[0.7,0.3],[0.2,0.3],[0.2,0.8]]]}",
  )
  def test_geom(self, clip_geom: str) -> None:
    src_ds = self._make_mem_polygon_ds([
        "POLYGON ((0 0,0 -1,-1 -1,-1 0,0 0))",
        "POLYGON ((0 0,0 1,1 1,1 0,0 0))",
    ])

    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--geometry", clip_geom, "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

  @parameterized.parameters(
      "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))",
      '{"type":"Polygon","coordinates":'
      "[[[0.2,0.8],[0.7,0.8],[0.7,0.3],[0.2,0.3],[0.2,0.8]]]}",
  )
  def test_geom_srs(self, clip_geom: str) -> None:
    srs_wgs84 = osr.SpatialReference()
    srs_wgs84.SetFromUserInput("WGS84")
    srs_wgs84.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    src_ds = self._make_mem_polygon_ds(
        ["POLYGON ((0 0,0 1,1 1,1 0,0 0))"], srs=srs_wgs84
    )

    clip_geom = (
        clip_geom.replace("0.2", "22263.8981586547")
        .replace("0.3", "33395.9998333802")
        .replace("0.7", "77923.6435552915")
        .replace("0.8", "89058.4864167096")
    )

    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments([
            "--geometry",
            clip_geom,
            "--geometry-crs",
            "EPSG:3857",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

  def test_geom_not_rectangle_and_type_handling(self) -> None:
    src_ds = self._make_mem_polygon_ds([
        "POLYGON ((0.4 0.4,0.4 0.5,0.5 0.5,0.4 0.4))",
        "POLYGON ((-1 -1,-1 2,2 2,2 -1,-1 -1))",
    ])
    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments([
            "--geometry",
            "POLYGON ((0 0,0 1,1 1,1 0,0.8 0,0.8 0.8,0.2 0.8,0.2 0,0 0))",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0 0,0 1,1 1,1 0,0.8 0,0.8 0.8,0.2 0.8,0.2 0,0 0))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    # Incompatible geometry type
    line_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    line_lyr = line_ds.CreateLayer("test", geom_type=ogr.wkbLineString)
    f = ogr.Feature(line_lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("LINESTRING(1 1,2 2)"))
    line_lyr.CreateFeature(f)

    clip = _get_clip_alg()
    clip["input"] = line_ds
    self.assertTrue(
        clip.ParseCommandLineArguments([
            "--geometry",
            "POLYGON ((0 0,0 1,1 1,1 0,0 0))",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    self.assertIsNone(out_ds.GetLayer(0).GetNextFeature())

    # Promote simple geometry to MultiPolygon
    mp_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    mp_lyr = mp_ds.CreateLayer("test", geom_type=ogr.wkbMultiPolygon)
    f = ogr.Feature(mp_lyr.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "MULTIPOLYGON(((-0.5 -0.5,-0.5 1.5,1.5 1.5,1.5 -0.5,-0.5 -0.5)))"
        )
    )
    mp_lyr.CreateFeature(f)

    clip = _get_clip_alg()
    clip["input"] = mp_ds
    self.assertTrue(
        clip.ParseCommandLineArguments([
            "--geometry",
            "POLYGON ((0 0,0 1,1 1,1 0,0 0))",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_f = out_ds.GetLayer(0).GetNextFeature()
    self._assert_geom_equal(out_f, "MULTIPOLYGON (((0 1,1 1,1 0,0 0,0 1)))")

  def test_like_vector_variants(self) -> None:
    src_ds = self._make_mem_polygon_ds(["POLYGON ((0 0,0 1,1 1,1 0,0 0))"])
    like_ds = self._make_mem_polygon_ds(
        ["POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"]
    )

    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_ds
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    # Invalid geometry in like dataset
    bad_like = self._make_mem_polygon_ds(
        ["POLYGON ((0 0,1 1,0 1,1 0,0 0))"], ds_name="clip_ds"
    )
    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = bad_like
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    with gdal.quiet_errors(), self.assertRaisesRegex(
        RuntimeError, "Geometry of feature 0 of clip_ds is invalid"
    ):
      clip.Run()

  def test_like_vector_no_srs_warning(self) -> None:
    srs = osr.SpatialReference()
    srs.SetFromUserInput("WGS84")
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    src_ds = self._make_mem_polygon_ds(
        ["POLYGON ((0 0,0 1,1 1,1 0,0 0))"], srs=srs
    )
    like_ds = gdal.GetDriverByName("MEM").Create("clip_ds", 1, 1)
    like_ds.SetGeoTransform([0, 1, 0, 0, 0, 1])

    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["output"] = ""
    clip["output-format"] = "MEM"
    clip["like"] = like_ds
    with self._assert_warning_raised(
        "ataset 'clip_ds' has no CRS. Assuming its CRS is the same as the"
        " input vector"
    ):
      self.assertTrue(clip.Run())

  def test_like_layer_sql_and_where(self) -> None:
    src_ds = self._make_mem_polygon_ds(["POLYGON ((0 0,0 1,1 1,1 0,0 0))"])

    like_multi = gdal.GetDriverByName("MEM").Create(
        "", 0, 0, 0, gdal.GDT_Unknown
    )
    like_multi.CreateLayer("unused")
    like_lyr = like_multi.CreateLayer("my_layer")
    f = ogr.Feature(like_lyr.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
        )
    )
    like_lyr.CreateFeature(f)

    for args in (
        ["--like-layer", "my_layer", "--of", "MEM", "--output", "memory_ds"],
        [
            "--like-sql",
            "SELECT * FROM my_layer",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ],
    ):
      clip = _get_clip_alg()
      clip["input"] = src_ds
      clip["like"] = like_multi
      self.assertTrue(clip.ParseCommandLineArguments(args))
      self.assertTrue(clip.Run())
      out_ds = utilities_util.get_arg_dataset(clip, "output")
      out_lyr = out_ds.GetLayer(0)
      out_f = out_lyr.GetNextFeature()
      self.assertEqual(out_f["foo"], "bar")
      self._assert_geom_equal(
          out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
      )
      self.assertIsNone(out_lyr.GetNextFeature())

    # Invalid like-layer
    like_single = self._make_mem_polygon_ds([])
    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_single
    self.assertTrue(
        clip.ParseCommandLineArguments([
            "--like-layer",
            "invalid",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    with self.assertRaisesRegex(
        RuntimeError, "Failed to identify source layer from clipping dataset."
    ):
      clip.Run()

    # like-where
    like_where = gdal.GetDriverByName("MEM").Create(
        "", 0, 0, 0, gdal.GDT_Unknown
    )
    lyr = like_where.CreateLayer("my_layer")
    lyr.CreateField(ogr.FieldDefn("id", ogr.OFTInteger))
    for feat_id, wkt in (
        (0, "POLYGON ((0 0,0 1,1 1,1 0,0 0))"),
        (1, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"),
    ):
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat["id"] = feat_id
      feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(feat)

    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_where
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--like-where", "id=1", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    # Empty like-where result
    like_empty = gdal.GetDriverByName("MEM").Create(
        "", 0, 0, 0, gdal.GDT_Unknown
    )
    lyr_empty = like_empty.CreateLayer("my_layer")
    lyr_empty.CreateField(ogr.FieldDefn("id", ogr.OFTInteger))
    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_empty
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--like-where", "id=1", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    with self.assertRaisesRegex(RuntimeError, "No clipping geometry found"):
      clip.Run()

  def test_like_vector_srs_and_like_raster(self) -> None:
    srs_wgs84 = osr.SpatialReference()
    srs_wgs84.SetFromUserInput("WGS84")
    srs_wgs84.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    srs_3857 = osr.SpatialReference()
    srs_3857.SetFromUserInput("EPSG:3857")
    srs_3857.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    src_srs_ds = self._make_mem_polygon_ds(
        ["POLYGON ((0 0,0 1,1 1,1 0,0 0))"], srs=srs_wgs84
    )
    wkt = (
        "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))".replace(
            "0.2", "22263.8981586547"
        )
        .replace("0.3", "33395.9998333802")
        .replace("0.7", "77923.6435552915")
        .replace("0.8", "89058.4864167096")
    )
    like_srs_ds = self._make_mem_polygon_ds([wkt], srs=srs_3857)

    clip = _get_clip_alg()
    clip["input"] = src_srs_ds
    clip["like"] = like_srs_ds
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    self._assert_geom_equal(
        out_lyr.GetNextFeature(),
        "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))",
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    # Like raster without SRS
    src_no_srs = self._make_mem_polygon_ds(["POLYGON ((0 0,0 1,1 1,1 0,0 0))"])
    like_rast = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    like_rast.SetGeoTransform([0.2, 0.5, 0, 0.3, 0, 0.5])

    clip = _get_clip_alg()
    clip["input"] = src_no_srs
    clip["like"] = like_rast
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    # Like raster with SRS
    like_rast_srs = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    like_rast_srs.SetGeoTransform([
        22263.8981586547,
        55660.4518654215,
        0,
        33395.9998333802,
        0,
        55659.7453966368,
    ])
    like_rast_srs.SetSpatialRef(srs_3857)

    clip = _get_clip_alg()
    clip["input"] = src_srs_ds
    clip["like"] = like_rast_srs
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    self.assertTrue(clip.Run())
    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

  def test_errors(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_clip_err") as tmp:
      clip = _get_clip_alg()
      with self.assertRaisesRegex(
          RuntimeError, "clip: --bbox, --geometry or --like must be specified"
      ):
        clip.ParseRunAndFinalize([poly_shp, f"{tmp}/out.shp"])

    src_ds = self._make_mem_polygon_ds([])

    clip = _get_clip_alg()
    clip["input"] = src_ds
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--geometry", "invalid", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    with self.assertRaisesRegex(
        RuntimeError,
        "clip: Clipping geometry is neither a valid WKT or GeoJSON geometry",
    ):
      clip.Run()

    like_no_gt = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_no_gt
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    with self.assertRaisesRegex(
        RuntimeError,
        "clip: Dataset '' has no geotransform matrix. Its bounds cannot be"
        " established",
    ):
      clip.Run()

    like_two_lyrs = gdal.GetDriverByName("MEM").Create(
        "", 0, 0, 0, gdal.GDT_Unknown
    )
    like_two_lyrs.CreateLayer("test1")
    like_two_lyrs.CreateLayer("test2")
    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_two_lyrs
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    with self.assertRaisesRegex(
        RuntimeError,
        "clip: Only single layer dataset can be specified with --like",
    ):
      clip.Run()

    like_neither = gdal.GetDriverByName("MEM").Create(
        "clip", 0, 0, 0, gdal.GDT_Unknown
    )
    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["like"] = like_neither
    self.assertTrue(
        clip.ParseCommandLineArguments(["--of", "MEM", "--output", "memory_ds"])
    )
    with self.assertRaisesRegex(
        RuntimeError, "clip: Cannot get extent from clip dataset"
    ):
      clip.Run()

  def test_dataset_getnextfeature(self) -> None:
    if not utilities_util.has_driver("OSM"):
      self.skipTest("OSM driver not available")

    test_pbf = utilities_util.get_testdata_path("../ogr/data/osm/test.pbf")
    src_ds = gdal.OpenEx(test_pbf)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None

    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["bbox"] = [-180, -90, 180, 90]
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--of", "stream", "--output", "streamed_output"]
        )
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    self.assertTrue(out_ds.TestCapability(ogr.ODsCRandomLayerRead))

    expected = []
    while True:
      f, lyr = src_ds.GetNextFeature()
      if not f:
        break
      expected.append((f, lyr))

    got = []
    out_ds.ResetReading()
    while True:
      f, lyr = out_ds.GetNextFeature()
      if not f:
        break
      got.append((f, lyr))

    self.assertLen(got, len(expected))

  def test_bbox_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    src_lyr.CreateField(ogr.FieldDefn("foo"))
    for wkt in (
        "POLYGON ((0 0,0 -1,-1 -1,-1 0,0 0))",
        "POLYGON ((0 0,0 1,1 1,1 0,0 0))",
    ):
      f = ogr.Feature(src_lyr.GetLayerDefn())
      f["foo"] = "bar"
      f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      src_lyr.CreateFeature(f)

    other_lyr = src_ds.CreateLayer("other_layer")
    f = ogr.Feature(other_lyr.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt("POLYGON ((10 10,10 11,11 11,10 10))")
    )
    other_lyr.CreateFeature(f)

    clip = _get_clip_alg()
    clip["input"] = src_ds
    clip["active-layer"] = "the_layer"
    self.assertTrue(
        clip.ParseCommandLineArguments(
            ["--bbox", "0.2,0.3,0.7,0.8", "--of", "MEM", "--output", "mem_ds"]
        )
    )
    self.assertTrue(clip.Run())

    out_ds = utilities_util.get_arg_dataset(clip, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f["foo"], "bar")
    self._assert_geom_equal(
        out_f, "POLYGON ((0.2 0.8,0.7 0.8,0.7 0.3,0.2 0.3,0.2 0.8))"
    )
    self.assertIsNone(out_lyr.GetNextFeature())

    out_lyr_1 = out_ds.GetLayer(1)
    self._assert_geom_equal(
        out_lyr_1.GetNextFeature(), "POLYGON ((10 10,10 11,11 11,10 10))"
    )

  def test_test_ogrsf(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")

    test_ogrsf = os.path.join(
        os.path.dirname(utilities_util.get_gdal_path()), "test_ogrsf"
    )
    if not os.path.exists(test_ogrsf):
      self.skipTest("test_ogrsf binary not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      gdalg_filename = os.path.join(tmpdir, "tmp.gdalg.json")
      with open(gdalg_filename, "w", encoding="utf-8") as f:
        json.dump(
            {
                "type": "gdal_streamed_alg",
                "command_line": (
                    f"gdal vector clip {poly_shp} --like {poly_shp}"
                    " --output-format=stream foo"
                ),
                "relative_paths_relative_to_this_file": False,
            },
            f,
        )

      res = subprocess.run(
          [test_ogrsf, "-ro", gdalg_filename],
          capture_output=True,
          text=True,
          check=False,
      )
      self.assertEqual(res.returncode, 0)
      self.assertIn("INFO", res.stdout)
      self.assertNotIn("ERROR", res.stdout)
      self.assertNotIn("FAILURE", res.stdout)


if __name__ == "__main__":
  absltest.main()
