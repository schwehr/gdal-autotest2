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

"""Tests for `gdal vector edit`.

Ported from upstream autotest/utilities/test_gdalalg_vector_edit.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_edit_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["edit"]


class GdalalgVectorEditTest(gdaltest.TestCase):

  def _assert_point_equals(
      self, geom: ogr.Geometry, expected_x: float, expected_y: float
  ) -> None:
    self.assertEqual(geom.GetGeometryName(), "POINT")
    self.assertAlmostEqual(geom.GetX(), expected_x, delta=1e-6)
    self.assertAlmostEqual(geom.GetY(), expected_y, delta=1e-6)

  def test_crs(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr = src_ds.CreateLayer("the_layer")
    lyr.CreateField(ogr.FieldDefn("foo"))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["foo"] = "bar"
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
    lyr.CreateFeature(feat)

    alg = _get_edit_alg()
    alg["input"] = src_ds
    self.assertTrue(
        alg.ParseCommandLineArguments(
            ["--crs=EPSG:4326", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetSpatialRef().GetAuthorityCode(None), "4326")
    out_feat = out_lyr.GetNextFeature()
    out_geom = out_feat.GetGeometryRef()
    self.assertEqual(
        out_geom.GetSpatialReference().GetAuthorityCode(None), "4326"
    )
    self.assertEqual(out_feat["foo"], "bar")
    self._assert_point_equals(out_geom, 1.0, 2.0)

  def test_crs_none(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    lyr = src_ds.CreateLayer("the_layer", srs=srs)
    lyr.CreateField(ogr.FieldDefn("foo"))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["foo"] = "bar"
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
    lyr.CreateFeature(feat)

    alg = _get_edit_alg()
    alg["input"] = src_ds
    self.assertTrue(
        alg.ParseCommandLineArguments(
            ["--crs=none", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertIsNone(out_lyr.GetSpatialRef())
    out_feat = out_lyr.GetNextFeature()
    out_geom = out_feat.GetGeometryRef()
    self.assertIsNone(out_geom.GetSpatialReference())
    self.assertEqual(out_feat["foo"], "bar")
    self._assert_point_equals(out_geom, 1.0, 2.0)

  def test_dataset_metadata(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_ds.SetMetadataItem("A", "B")
    src_ds.SetMetadataItem("B", "C")

    alg = _get_edit_alg()
    alg["input"] = src_ds
    self.assertTrue(
        alg.ParseCommandLineArguments([
            "--metadata=C=D",
            "--unset-metadata=B",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetMetadata_Dict(), {"A": "B", "C": "D"})

  def test_layer_metadata(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_layer = src_ds.CreateLayer("the_layer")
    src_layer.SetMetadataItem("A", "B")
    src_layer.SetMetadataItem("B", "C")

    alg = _get_edit_alg()
    alg["input"] = src_ds
    self.assertTrue(
        alg.ParseCommandLineArguments([
            "--layer-metadata=C=D",
            "--unset-layer-metadata=B",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        out_ds.GetLayer(0).GetMetadata_Dict(), {"A": "B", "C": "D"}
    )

  def test_geometry_type_geometry(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_ds.CreateLayer("the_layer", geom_type=ogr.wkbPoint)

    alg = _get_edit_alg()
    alg["input"] = src_ds
    self.assertTrue(
        alg.ParseCommandLineArguments(
            ["--geometry-type=geometry", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetLayer(0).GetGeomType(), ogr.wkbUnknown)

  def test_geometry_type_invalid(self) -> None:
    alg = _get_edit_alg()
    with self.assertRaisesRegex(
        RuntimeError, "edit: Invalid geometry type 'invalid'"
    ):
      alg["geometry-type"] = "invalid"

  def test_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_ds.CreateLayer("the_layer")
    src_ds.CreateLayer("other_layer")

    alg = _get_edit_alg()
    alg["input"] = src_ds
    alg["active-layer"] = "the_layer"
    self.assertTrue(
        alg.ParseCommandLineArguments(
            ["--geometry-type=point", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetLayer(0).GetGeomType(), ogr.wkbPoint)
    self.assertEqual(out_ds.GetLayer(1).GetGeomType(), ogr.wkbUnknown)

  def test_unset_fid(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr = src_ds.CreateLayer("the_layer", options=["FID=my_fid_column"])
    lyr.CreateField(ogr.FieldDefn("foo"))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetFID(10)
    lyr.CreateFeature(feat)

    with gdal.Run("vector", "edit", input=src_ds, output_format="MEM") as alg:
      ds = alg.Output()
      out_lyr = ds.GetLayer(0)
      self.assertEqual(out_lyr.GetFIDColumn(), "my_fid_column")
      out_feat = out_lyr.GetNextFeature()
      self.assertEqual(out_feat.GetFID(), 10)

    with gdal.Run(
        "vector", "edit", input=src_ds, output_format="MEM", unset_fid=True
    ) as alg:
      ds = alg.Output()
      out_lyr = ds.GetLayer(0)
      self.assertEqual(out_lyr.GetFIDColumn(), "")
      out_feat = out_lyr.GetNextFeature()
      self.assertEqual(out_feat.GetFID(), 0)


if __name__ == "__main__":
  absltest.main()
