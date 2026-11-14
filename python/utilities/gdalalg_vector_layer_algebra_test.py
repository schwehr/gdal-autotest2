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

"""Tests for `gdal vector layer-algebra`.

Ported from upstream autotest/utilities/test_gdalalg_vector_layer_algebra.py.

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


def _create_empty_mem_ds() -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").CreateVector("")
  ds.CreateLayer("")
  return ds


def _get_input_method_datasets() -> tuple[gdal.Dataset, gdal.Dataset]:
  mem_drv = gdal.GetDriverByName("MEM")

  input_ds = mem_drv.CreateVector("")
  input_lyr = input_ds.CreateLayer("input")
  input_lyr.CreateField(ogr.FieldDefn("a"))
  for val, wkt in (
      ("foo", "POLYGON((0 0,10 0,10 10,0 10,0 0))"),
      ("foo2", "POINT(-3 4)"),
  ):
    feat = ogr.Feature(input_lyr.GetLayerDefn())
    feat["a"] = val
    feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
    input_lyr.CreateFeature(feat)

  method_ds = mem_drv.CreateVector("")
  method_lyr = method_ds.CreateLayer("method")
  method_lyr.CreateField(ogr.FieldDefn("b"))
  for val, wkt in (
      ("bar", "POLYGON((5 0,15 0,15 10,5 10,5 0))"),
      ("bar2", "POINT(-3 4)"),
      ("bar3", "POINT(-5 6)"),
  ):
    feat = ogr.Feature(method_lyr.GetLayerDefn())
    feat["b"] = val
    feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
    method_lyr.CreateFeature(feat)

  return input_ds, method_ds


class GdalalgVectorLayerAlgebraTest(gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if ogr.GetGEOSVersionMajor() == 0:
      self.skipTest("GEOS not available")

  def test_union(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    input_ds = mem_drv.CreateVector("")
    input_lyr = input_ds.CreateLayer("input")
    input_lyr.CreateField(ogr.FieldDefn("a"))
    input_lyr.CreateField(ogr.FieldDefn("ignored"))
    for val, wkt in (("foo", "POINT(1 2)"), ("foo2", "POINT(3 4)")):
      feat = ogr.Feature(input_lyr.GetLayerDefn())
      feat["a"] = val
      feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      input_lyr.CreateFeature(feat)

    method_ds = mem_drv.CreateVector("")
    method_lyr = method_ds.CreateLayer("method")
    method_lyr.CreateField(ogr.FieldDefn("b"))
    method_lyr.CreateField(ogr.FieldDefn("ignored"))
    for val, wkt in (("bar", "POINT(5 6)"), ("bar2", "POINT(3 4)")):
      feat = ogr.Feature(method_lyr.GetLayerDefn())
      feat["b"] = val
      feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      method_lyr.CreateFeature(feat)

    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="union",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
        geometry_type="MULTIPOINT",
        input_field=["a", "non_existing"],
        method_field=["b", "non_existing"],
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "input_a"
      )
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(1).GetName(), "method_b"
      )
      self.assertEqual(out_lyr.GetFeatureCount(), 3)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo")
      self.assertFalse(feat.IsFieldSet("method_b"))
      self.assertEqual(
          feat.GetGeometryRef().ExportToIsoWkt(), "MULTIPOINT ((1 2))"
      )

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo2")
      self.assertEqual(feat["method_b"], "bar2")
      self.assertEqual(
          feat.GetGeometryRef().ExportToIsoWkt(), "MULTIPOINT ((3 4))"
      )

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["method_b"], "bar")
      self.assertFalse(feat.IsFieldSet("input_a"))
      self.assertEqual(
          feat.GetGeometryRef().ExportToIsoWkt(), "MULTIPOINT ((5 6))"
      )

  def test_input_and_method_same(self) -> None:
    ds = gdal.GetDriverByName("MEM").CreateVector("")
    with self.assertRaisesRegex(
        RuntimeError, "Input and method datasets must be different"
    ):
      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=ds,
          method=ds,
          output_format="MEM",
      )

  def test_cannot_create_output(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with self.assertRaises(RuntimeError):
      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=_create_empty_mem_ds(),
          method=_create_empty_mem_ds(),
          output="/i_do/not/exist.gpkg",
      )

  def test_cannot_create_layer(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with utilities_util.vsimem_dir("layer_algebra_bad_layer") as tmp:
      with self.assertRaisesRegex(
          RuntimeError, "name may not contain special characters or spaces"
      ):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=_create_empty_mem_ds(),
            method=_create_empty_mem_ds(),
            output=f"{tmp}/out.gpkg",
            layer_creation_option={"FID": "~!@#$%^&*()"},
        )

  def test_cannot_create_field_from_input(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    input_ds = _create_empty_mem_ds()
    input_ds.GetLayer(0).CreateField(ogr.FieldDefn("geom"))
    with utilities_util.vsimem_dir("layer_algebra_bad_in_field") as tmp:
      with self.assertRaisesRegex(RuntimeError, "Cannot create field geom"):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=input_ds,
            method=_create_empty_mem_ds(),
            output=f"{tmp}/out.gpkg",
            input_prefix="",
        )

  def test_cannot_create_field_from_method(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    method_ds = _create_empty_mem_ds()
    method_ds.GetLayer(0).CreateField(ogr.FieldDefn("geom"))
    with utilities_util.vsimem_dir("layer_algebra_bad_method_field") as tmp:
      with self.assertRaisesRegex(RuntimeError, "Cannot create field geom"):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=_create_empty_mem_ds(),
            method=method_ds,
            output=f"{tmp}/out.gpkg",
            method_prefix="",
        )

  def test_cannot_guess_output_format(self) -> None:
    with utilities_util.vsimem_dir("layer_algebra_noguess") as tmp:
      with self.assertRaisesRegex(RuntimeError, "Cannot guess driver"):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=_create_empty_mem_ds(),
            method=_create_empty_mem_ds(),
            output=f"{tmp}/out.xxx",
        )

  def test_cannot_get_input_and_method_layers(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("layer_algebra_missing_lyr") as tmp:
      with self.assertRaisesRegex(RuntimeError, "Cannot get input layer 'foo'"):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=_create_empty_mem_ds(),
            method=_create_empty_mem_ds(),
            input_layer="foo",
            output=f"{tmp}/out.shp",
        )

      with self.assertRaisesRegex(
          RuntimeError, "Cannot get method layer 'foo'"
      ):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=_create_empty_mem_ds(),
            method=_create_empty_mem_ds(),
            method_layer="foo",
            output=f"{tmp}/out.shp",
        )

  def test_overwrite(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("layer_algebra_overwrite") as tmp:
      out_shp = f"{tmp}/out.shp"
      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_shp,
          no_input_field=True,
          no_method_field=True,
      )

      with self.assertRaisesRegex(RuntimeError, "already exists"):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=poly_shp,
            method=poly_shp,
            output=out_shp,
            no_input_field=True,
            no_method_field=True,
        )

      with self.assertRaisesRegex(
          RuntimeError, "--output-layer should be specified"
      ):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=poly_shp,
            method=poly_shp,
            output=out_shp,
            update=True,
            no_input_field=True,
            no_method_field=True,
        )

      with gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_shp,
          append=True,
          no_input_field=True,
          no_method_field=True,
      ) as alg:
        out_ds = alg.Output()
        self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 20)
        out_ds = None

      with gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_shp,
          overwrite=True,
          no_input_field=True,
          no_method_field=True,
      ) as alg:
        out_ds = alg.Output()
        self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 10)
        out_ds = None

      with gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_shp,
          append=True,
          no_input_field=True,
          no_method_field=True,
      ) as alg:
        out_ds = alg.Output()
        self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 20)
        out_ds = None

      with gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_shp,
          overwrite_layer=True,
          no_input_field=True,
          no_method_field=True,
      ) as alg:
        out_ds = alg.Output()
        self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 10)
        out_ds = None

  def test_delete_layer_single_fail(self) -> None:
    self.skipTest(
        "Upstream OGRShapeDataSource::SyncToDisk std::sort comparator is not "
        "strict-weak-ordering under libc++ hardening; see "
        "ogrshapedatasource.cpp:1629"
    )

  def test_output_driver_not_existing(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("layer_algebra_nodrv") as tmp:
      alg = gdal.Algorithm("vector", "layer-algebra")
      alg.ParseCommandLineArguments([
          "union",
          poly_shp,
          poly_shp,
          f"{tmp}/out.shp",
          "--output-format",
          "ESRI Shapefile",
      ])

      drv = gdal.GetDriverByName("ESRI Shapefile")
      drv.Deregister()
      try:
        with self.assertRaisesRegex(
            RuntimeError, "Driver 'ESRI Shapefile' does not exist"
        ):
          alg.Run()
      finally:
        drv.Register()

  def test_overwrite_multilayer(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("layer_algebra_multilayer") as tmp:
      out_gpkg = f"{tmp}/out.gpkg"
      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_gpkg,
      )
      with gdal.OpenEx(out_gpkg) as ds:
        self.assertEqual(ds.GetLayer(0).GetName(), "output")

      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_gpkg,
          update=True,
          output_layer="output2",
      )
      with gdal.OpenEx(out_gpkg, gdal.OF_UPDATE) as ds:
        self.assertEqual(ds.GetLayer(0).GetName(), "output")
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        self.assertEqual(ds.GetLayer(1).GetName(), "output2")
        self.assertEqual(ds.GetLayer(1).GetFeatureCount(), 10)

      for kwargs in (
          {"update": True},
          {"append": True},
          {"overwrite_layer": True},
      ):
        with self.assertRaisesRegex(
            RuntimeError, "--output-layer should be specified"
        ):
          gdal.Run(
              "vector",
              "layer-algebra",
              operation="union",
              input=poly_shp,
              method=poly_shp,
              output=out_gpkg,
              **kwargs,
          )

      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_gpkg,
          append=True,
          output_layer="output2",
      )
      with gdal.OpenEx(out_gpkg, gdal.OF_UPDATE) as ds:
        self.assertEqual(ds.GetLayer(0).GetName(), "output")
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        self.assertEqual(ds.GetLayer(1).GetName(), "output2")
        self.assertEqual(ds.GetLayer(1).GetFeatureCount(), 20)

      gdal.Run(
          "vector",
          "layer-algebra",
          operation="union",
          input=poly_shp,
          method=poly_shp,
          output=out_gpkg,
          overwrite_layer=True,
          output_layer="output2",
      )
      with gdal.OpenEx(out_gpkg, gdal.OF_UPDATE) as ds:
        self.assertEqual(ds.GetLayer(0).GetName(), "output")
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        self.assertEqual(ds.GetLayer(1).GetName(), "output2")
        self.assertEqual(ds.GetLayer(1).GetFeatureCount(), 10)

      for kwargs in (
          {"append": True, "output_layer": "wrong"},
          {"overwrite_layer": True, "output_layer": "wrong"},
      ):
        with self.assertRaisesRegex(
            RuntimeError, "Layer 'wrong' does not exist"
        ):
          gdal.Run(
              "vector",
              "layer-algebra",
              operation="union",
              input=poly_shp,
              method=poly_shp,
              output=out_gpkg,
              **kwargs,
          )

      with self.assertRaisesRegex(
          RuntimeError,
          r"Output layer 'output2' already exists\. Specify --overwrite,"
          r" --overwrite-layer, --append or --update \+ --output-layer with a"
          r" different name",
      ):
        gdal.Run(
            "vector",
            "layer-algebra",
            operation="union",
            input=poly_shp,
            method=poly_shp,
            output=out_gpkg,
            update=True,
            output_layer="output2",
        )

  def test_intersection(self) -> None:
    input_ds, method_ds = _get_input_method_datasets()
    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="intersection",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "input_a"
      )
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(1).GetName(), "method_b"
      )
      self.assertEqual(out_lyr.GetFeatureCount(), 2)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo")
      self.assertEqual(feat["method_b"], "bar")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((5 0,5 10,10 10,10 0,5 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo2")
      self.assertEqual(feat["method_b"], "bar2")
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (-3 4)")

  def test_sym_difference(self) -> None:
    input_ds, method_ds = _get_input_method_datasets()
    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="sym-difference",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "input_a"
      )
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(1).GetName(), "method_b"
      )
      self.assertEqual(out_lyr.GetFeatureCount(), 3)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo")
      self.assertIsNone(feat["method_b"])
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((0 0,0 10,5 10,5 0,0 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertIsNone(feat["input_a"])
      self.assertEqual(feat["method_b"], "bar")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((15 10,15 0,10 0,10 10,15 10))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertIsNone(feat["input_a"])
      self.assertEqual(feat["method_b"], "bar3")
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (-5 6)")

  def test_identity(self) -> None:
    input_ds, method_ds = _get_input_method_datasets()
    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="identity",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "input_a"
      )
      self.assertEqual(
          out_lyr.GetLayerDefn().GetFieldDefn(1).GetName(), "method_b"
      )
      self.assertEqual(out_lyr.GetFeatureCount(), 3)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo")
      self.assertEqual(feat["method_b"], "bar")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((5 0,5 10,10 10,10 0,5 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo")
      self.assertIsNone(feat["method_b"])
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((0 0,0 10,5 10,5 0,0 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["input_a"], "foo2")
      self.assertEqual(feat["method_b"], "bar2")
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (-3 4)")

  def test_update(self) -> None:
    input_ds, method_ds = _get_input_method_datasets()
    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="update",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "a")
      self.assertEqual(out_lyr.GetFeatureCount(), 4)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["a"], "foo")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((0 0,0 10,5 10,5 0,0 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertIsNone(feat["a"])
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((5 0,15 0,15 10,5 10,5 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertIsNone(feat["a"])
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (-3 4)")

      feat = out_lyr.GetNextFeature()
      self.assertIsNone(feat["a"])
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (-5 6)")

  def test_clip(self) -> None:
    input_ds, method_ds = _get_input_method_datasets()
    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="clip",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "a")
      self.assertEqual(out_lyr.GetFeatureCount(), 2)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["a"], "foo")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((5 0,5 10,10 10,10 0,5 0))",
      )

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["a"], "foo2")
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (-3 4)")

  def test_erase(self) -> None:
    input_ds, method_ds = _get_input_method_datasets()
    with gdal.Run(
        "vector",
        "layer-algebra",
        operation="erase",
        input=input_ds,
        method=method_ds,
        output_format="MEM",
    ) as alg:
      out_ds = alg.Output()
      out_lyr = out_ds.GetLayer(0)
      self.assertEqual(out_lyr.GetName(), "output")
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(out_lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "a")
      self.assertEqual(out_lyr.GetFeatureCount(), 1)

      feat = out_lyr.GetNextFeature()
      self.assertEqual(feat["a"], "foo")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((0 0,0 10,5 10,5 0,0 0))",
      )


if __name__ == "__main__":
  absltest.main()
