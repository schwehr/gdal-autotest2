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

"""Tests for `gdal vector select`.

Ported from upstream autotest/utilities/test_gdalalg_vector_select.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_select_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["select"]


class GdalalgVectorSelectTest(gdaltest.TestCase):

  def test_fields(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    select_alg = _get_select_alg()
    self.assertTrue(
        select_alg.ParseCommandLineArguments([
            "--fields=EAS_ID,_ogr_geometry_",
            "--of=stream",
            poly_shp,
            "streamed_output",
        ])
    )
    self.assertTrue(select_alg.Run())

    ds = select_alg.GetArg("output").Get().GetDataset()
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
    self.assertEqual(lyr.GetFeatureCount(), 10)

    feat = lyr.GetNextFeature()
    self.assertEqual(feat["EAS_ID"], 168)
    self.assertIsNotNone(feat.GetGeometryRef())

    lyr.ResetReading()
    self.assertLen([f for f in lyr], 10)

    feat = lyr.GetFeature(0)
    self.assertEqual(feat["EAS_ID"], 168)
    with self.assertRaises(RuntimeError):
      lyr.GetFeature(10)

    self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
    self.assertEqual(lyr.TestCapability(ogr.OLCRandomWrite), 0)
    self.assertEqual(
        lyr.GetExtent(), (478315.53125, 481645.3125, 4762880.5, 4765610.5)
    )
    self.assertEqual(
        lyr.GetExtent(0), (478315.53125, 481645.3125, 4762880.5, 4765610.5)
    )

    lyr.SetAttributeFilter("EAS_ID = 170")
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.assertEqual(feat["EAS_ID"], 170)
    self.assertIsNone(lyr.GetNextFeature())
    self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
    self.assertEqual(lyr.GetFeatureCount(), 1)
    lyr.SetAttributeFilter(None)

    lyr.SetSpatialFilterRect(-1, -1, -1, -1)
    lyr.ResetReading()
    self.assertIsNone(lyr.GetNextFeature())
    self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
    self.assertEqual(lyr.GetFeatureCount(), 0)
    lyr.SetSpatialFilter(None)

  def test_fields_geom_named(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbNone, srs=None)
    src_lyr.CreateGeomField(ogr.GeomFieldDefn("geom_field"))
    src_lyr.CreateGeomField(ogr.GeomFieldDefn("geom_field2"))

    with utilities_util.vsimem_dir("vector_select_geom_named") as vsimem:
      out_filename = f"{vsimem}/out.shp"
      select_alg = _get_select_alg()
      select_alg["input"] = src_ds
      select_alg["output"] = out_filename
      select_alg["fields"] = ["geom_field2"]
      self.assertTrue(select_alg.ParseCommandLineArguments(["--of", "MEM"]))
      self.assertTrue(select_alg.Run())

      ds = utilities_util.get_arg_dataset(select_alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
      self.assertEqual(
          lyr.GetLayerDefn().GetGeomFieldDefn(0).GetName(), "geom_field2"
      )

  def test_fields_non_existing(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_select_non_existing") as vsimem:
      out_filename = f"{vsimem}/out.shp"
      select_alg = _get_select_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "Field 'i_do_not_exist' does not exist in layer 'poly'. "
          "You may specify --ignore-missing-fields to skip it",
      ):
        select_alg.ParseRunAndFinalize(
            ["--fields=EAS_ID,i_do_not_exist", poly_shp, out_filename]
        )

  def test_fields_non_existing_ignore_missing_fields(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_select_ignore_missing") as vsimem:
      out_filename = f"{vsimem}/out.shp"
      select_alg = _get_select_alg()
      with gdal.quiet_errors():
        self.assertTrue(
            select_alg.ParseRunAndFinalize([
                "--ignore-missing-fields",
                "--fields=EAS_ID,_ogr_geometry_,i_do_not_exist",
                poly_shp,
                out_filename,
            ])
        )

      with gdal.OpenEx(out_filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)

  def test_fields_exclude(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_select_exclude") as vsimem:
      out_filename = f"{vsimem}/out.shp"
      select_alg = _get_select_alg()
      self.assertTrue(
          select_alg.ParseRunAndFinalize([
              "--exclude",
              "--fields=EAS_ID,i_do_not_exist",
              poly_shp,
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        lyr = ds.GetLayer(0)
        lyr_defn = lyr.GetLayerDefn()
        self.assertEqual(
            [
                lyr_defn.GetFieldDefn(i).GetName()
                for i in range(lyr_defn.GetFieldCount())
            ],
            ["AREA", "PRFEDEA"],
        )
        self.assertEqual(lyr_defn.GetGeomFieldCount(), 1)

  def test_fields_exclude_ogr_geometry(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_select_exclude_geom") as vsimem:
      out_filename = f"{vsimem}/out.dbf"
      select_alg = _get_select_alg()
      self.assertTrue(
          select_alg.ParseRunAndFinalize(
              ["--exclude", "--fields=_ogr_geometry_", poly_shp, out_filename]
          )
      )

      with gdal.OpenEx(out_filename) as ds:
        lyr = ds.GetLayer(0)
        lyr_defn = lyr.GetLayerDefn()
        self.assertEqual(
            [
                lyr_defn.GetFieldDefn(i).GetName()
                for i in range(lyr_defn.GetFieldCount())
            ],
            ["AREA", "EAS_ID", "PRFEDEA"],
        )
        self.assertEqual(lyr_defn.GetGeomFieldCount(), 0)

  def test_fields_exclude_name_geom_fields(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_select_exclude_gpkg") as vsimem:
      tmp_filename = f"{vsimem}/tmp.gpkg"
      out_filename = f"{vsimem}/out.dbf"
      gdal.VectorTranslate(tmp_filename, poly_shp)

      select_alg = _get_select_alg()
      self.assertTrue(
          select_alg.ParseRunAndFinalize(
              ["--exclude", "--fields=geom", tmp_filename, out_filename]
          )
      )

      with gdal.OpenEx(out_filename) as ds:
        lyr = ds.GetLayer(0)
        lyr_defn = lyr.GetLayerDefn()
        self.assertEqual(
            [
                lyr_defn.GetFieldDefn(i).GetName()
                for i in range(lyr_defn.GetFieldCount())
            ],
            ["AREA", "EAS_ID", "PRFEDEA"],
        )
        self.assertEqual(lyr_defn.GetGeomFieldCount(), 0)

  def test_fields_exclude_name_geom_fields_not_excluded(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_select_not_excluded") as vsimem:
      tmp_filename = f"{vsimem}/tmp.gpkg"
      out_filename = f"{vsimem}/out.dbf"
      gdal.VectorTranslate(tmp_filename, poly_shp)

      select_alg = _get_select_alg()
      self.assertTrue(
          select_alg.ParseRunAndFinalize([
              "--exclude",
              "--fields=i_do_not_exist",
              tmp_filename,
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        lyr = ds.GetLayer(0)
        lyr_defn = lyr.GetLayerDefn()
        self.assertEqual(
            [
                lyr_defn.GetFieldDefn(i).GetName()
                for i in range(lyr_defn.GetFieldCount())
            ],
            ["AREA", "EAS_ID", "PRFEDEA"],
        )
        self.assertEqual(lyr_defn.GetGeomFieldCount(), 1)

  def test_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    src_lyr.CreateField(ogr.FieldDefn("a"))
    src_lyr.CreateField(ogr.FieldDefn("b"))
    src_lyr.CreateField(ogr.FieldDefn("c"))

    other_lyr = src_ds.CreateLayer("other_layer")
    other_lyr.CreateField(ogr.FieldDefn("b"))
    other_lyr.CreateField(ogr.FieldDefn("c"))

    select_alg = _get_select_alg()
    select_alg["input"] = src_ds
    select_alg["active-layer"] = "the_layer"
    self.assertTrue(
        select_alg.ParseCommandLineArguments(
            ["--fields=b", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(select_alg.Run())

    out_ds = utilities_util.get_arg_dataset(select_alg, "output")
    self.assertEqual(out_ds.GetLayer(0).GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(out_ds.GetLayer(1).GetLayerDefn().GetFieldCount(), 2)


if __name__ == "__main__":
  absltest.main()
