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

"""Tests for `gdal vector make-valid`.

Ported from upstream autotest/utilities/test_gdalalg_vector_make_valid.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Mapping
import json
import os
from typing import Any

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


def _have_geos() -> bool:
  return ogr.GetGEOSVersionMajor() > 0


def _have_geos_at_least(major: int, minor: int, micro: int = 0) -> bool:
  return _have_geos() and (
      ogr.GetGEOSVersionMajor(),
      ogr.GetGEOSVersionMinor(),
      ogr.GetGEOSVersionMicro(),
  ) >= (major, minor, micro)


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["make-valid"]


class GdalalgVectorMakeValidTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if not _have_geos():
      self.skipTest("GEOS not available")

  def _assert_feature_geom_wkt(
      self, feat: ogr.Feature, expected_wkt: str
  ) -> None:
    geom = feat.GetGeometryRef()
    self.assertIsNotNone(geom)
    assert geom is not None
    expected = ogr.CreateGeometryFromWkt(expected_wkt)
    self.assertIsNotNone(expected)
    assert expected is not None
    if expected.IsEmpty():
      self.assertTrue(geom.IsEmpty())
      self.assertEqual(geom.GetGeometryType(), expected.GetGeometryType())
    else:
      self.assertTrue(
          (
              not geom.Is3D()
              and not geom.IsMeasured()
              and geom.Within(expected)
              and expected.Within(geom)
          )
          or geom.Equals(expected),
          msg=f"Expected {expected_wkt}, got {geom.ExportToIsoWkt()}",
      )

  def test_make_valid(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    src_lyr = src_ds.CreateLayer("the_layer", srs=srs)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(
        ogr.CreateGeometryFromWkt("POLYGON ((0 0,1 1,1 0,0 1,0 0))")
    )
    src_lyr.CreateFeature(feat)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
    src_lyr.CreateFeature(feat)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)

    out_f = out_lyr.GetNextFeature()
    self.assertEqual(
        out_f.GetGeometryRef().GetGeometryType(), ogr.wkbMultiPolygon
    )
    self.assertEqual(
        out_f.GetGeometryRef().GetSpatialReference().GetAuthorityCode(None),
        "32631",
    )

    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f.GetGeometryRef().ExportToIsoWkt(), "POINT (1 2)")

    out_f = out_lyr.GetNextFeature()
    self.assertIsNone(out_f.GetGeometryRef())

    self.assertEqual(out_lyr.GetFeatureCount(), 3)
    out_lyr.SetAttributeFilter("0 = 1")
    self.assertEqual(out_lyr.GetFeatureCount(), 0)
    out_lyr.SetAttributeFilter(None)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCRandomWrite), 0)
    self.assertEqual(out_lyr.GetExtent(), (0.0, 1.0, 0.0, 2.0))
    self.assertEqual(out_lyr.GetFeature(0).GetFID(), 0)
    self.assertIsNone(out_lyr.GetFeature(-1))

  def test_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    for name in ("the_layer", "other_layer"):
      lyr = src_ds.CreateLayer(name)
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometry(
          ogr.CreateGeometryFromWkt("POLYGON ((0 0,1 1,1 0,0 1,0 0))")
      )
      lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["active-layer"] = "the_layer"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr0 = out_ds.GetLayer(0)
    out_f0 = out_lyr0.GetNextFeature()
    geom0 = out_f0.GetGeometryRef()
    self.assertIsNotNone(geom0)
    assert geom0 is not None
    self.assertEqual(geom0.GetGeometryType(), ogr.wkbMultiPolygon)

    out_lyr1 = out_ds.GetLayer(1)
    out_f1 = out_lyr1.GetNextFeature()
    geom1 = out_f1.GetGeometryRef()
    self.assertIsNotNone(geom1)
    assert geom1 is not None
    self.assertEqual(geom1.GetGeometryType(), ogr.wkbPolygon)

  def test_active_geometry(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer", geom_type=ogr.wkbNone)
    src_lyr.CreateGeomField(ogr.GeomFieldDefn("a"))
    src_lyr.CreateGeomField(ogr.GeomFieldDefn("b"))

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeomField(
        0, ogr.CreateGeometryFromWkt("POLYGON ((0 0,1 1,1 0,0 1,0 0))")
    )
    feat.SetGeomField(
        1, ogr.CreateGeometryFromWkt("POLYGON ((0 0,1 1,1 0,0 1,0 0))")
    )
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["active-geometry"] = "b"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(out_f.GetGeomFieldRef(0).GetGeometryType(), ogr.wkbPolygon)
    self.assertEqual(
        out_f.GetGeomFieldRef(1).GetGeometryType(), ogr.wkbMultiPolygon
    )

  def test_skip_and_keep_lower_dim(self) -> None:
    wkt = "POLYGON ((0 0,0 1,0.5 1,0.5 0.75,0.5 1,1 1,1 0,0 0))"

    def make_src() -> gdal.Dataset:
      ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
      lyr = ds.CreateLayer("the_layer")
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(feat)
      lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
      return ds

    src_ds = make_src()
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self._assert_feature_geom_wkt(
        out_f, "POLYGON ((0 0,0 1,0.5 1.0,1 1,1 0,0 0))"
    )

    src_ds_keep = make_src()
    alg_keep = _get_alg()
    alg_keep["input"] = src_ds_keep
    alg_keep["output"] = ""
    alg_keep["output-format"] = "stream"
    alg_keep["keep-lower-dim"] = True
    self.assertTrue(alg_keep.Run())
    out_ds_keep = utilities_util.get_arg_dataset(alg_keep, "output")
    out_lyr_keep = out_ds_keep.GetLayer(0)
    out_f_keep = out_lyr_keep.GetNextFeature()
    geom_keep = out_f_keep.GetGeometryRef()
    self.assertIsNotNone(geom_keep)
    assert geom_keep is not None
    self.assertEqual(geom_keep.GetGeometryType(), ogr.wkbGeometryCollection)

  @parameterized.parameters(
      ("LINESTRING (0 0,0 0)", {}, "POINT (0 0)"),
      ("LINESTRING (0 0,0 0)", {"method": "structure"}, "LINESTRING EMPTY"),
      (
          "LINESTRING (0 0,0 0)",
          {"method": "structure", "keep-lower-dim": True},
          "POINT (0 0)",
      ),
  )
  def test_options(
      self, input_wkt: str, options: Mapping[str, Any], output_wkt: str
  ) -> None:
    if not _have_geos_at_least(3, 10, 0):
      self.skipTest("GEOS >= 3.10.0 not available")

    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt(input_wkt))
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    for key, val in options.items():
      alg[key] = val
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_f = out_ds.GetLayer(0).GetNextFeature()
    self._assert_feature_geom_wkt(out_f, output_wkt)

  def test_gdalg_streamed(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": (
            f"gdal vector make-valid {poly_shp} --output-format=stream foo"
        ),
        "relative_paths_relative_to_this_file": False,
    }
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      gdalg_filename = os.path.join(tmpdir, "tmp.gdalg.json")
      with open(gdalg_filename, "wb") as f:
        f.write(json.dumps(spec).encode("utf-8"))
      with gdal.OpenEx(gdalg_filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)


if __name__ == "__main__":
  absltest.main()
