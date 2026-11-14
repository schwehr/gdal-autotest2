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

"""Tests for `gdal vector concat`.

Ported from upstream autotest/utilities/test_gdalalg_vector_concat.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence
import json
import os

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


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["concat"]


def _get_pipeline_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["pipeline"]


_POLY_PRJ_WKT = (
    'PROJCS["OSGB 1936 / British National Grid",'
    'GEOGCS["OSGB 1936",DATUM["D_OSGB_1936",'
    'SPHEROID["Airy_1830",6377563.396,299.3249646]],'
    'PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],'
    'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",49],'
    'PARAMETER["central_meridian",-2],PARAMETER["scale_factor",0.9996012717],'
    'PARAMETER["false_easting",400000],PARAMETER["false_northing",-100000],'
    'UNIT["Meter",1]]'
)


def _stage_poly_shp_with_prj(poly_shp: str, dst_dir: str) -> str:
  base = os.path.splitext(poly_shp)[0]
  for ext in (".shp", ".shx", ".dbf"):
    with open(base + ext, "rb") as f:
      gdal.FileFromMemBuffer(f"{dst_dir}/poly{ext}", f.read())
  prj_src = next(
      (base + ext for ext in (".PRJ", ".prj") if os.path.exists(base + ext)),
      None,
  )
  if prj_src is not None:
    with open(prj_src, "rb") as f:
      gdal.FileFromMemBuffer(f"{dst_dir}/poly.prj", f.read())
  else:
    gdal.FileFromMemBuffer(f"{dst_dir}/poly.prj", _POLY_PRJ_WKT.encode("ascii"))
  return f"{dst_dir}/poly.shp"


class GdalalgVectorConcatTest(gdaltest.TestCase, parameterized.TestCase):

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
        self.assertAlmostEqual(
            actual.GetZ(i), expected.GetZ(i), delta=max_error
        )

  @parameterized.parameters("1", None)
  def test_basic(self, max_opened_datasets: str | None) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    last_pct = [0.0]

    def my_progress(pct: float, _msg: str, _user_data: object) -> bool:
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir(
        f"vector_concat_basic_{max_opened_datasets}"
    ) as tmp:
      staged_poly = _stage_poly_shp_with_prj(poly_shp, tmp)
      alg = _get_alg()
      alg["input"] = [staged_poly, staged_poly]
      alg["output"] = ""
      alg["output-format"] = "MEM"
      with gdal.config_option(
          "GDAL_VECTOR_CONCAT_MAX_OPENED_DATASETS", max_opened_datasets
      ):
        self.assertTrue(alg.Run(my_progress))
      self.assertEqual(last_pct[0], 1.0)

      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayerCount(), 1)
      self.assertIsNone(ds.GetLayer(-1))
      self.assertIsNone(ds.GetLayer(1))
      lyr = ds.GetLayerByName("poly")
      srs = lyr.GetSpatialRef()
      self.assertIsNotNone(srs)
      assert srs is not None
      self.assertEqual(srs.GetAuthorityCode(None), "27700")
      self.assertEqual(lyr.GetFeatureCount(), 20)
      feat = lyr.GetNextFeature()
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
      self.assertEqual(feat["AREA"], 215229.266)
      self.assertEqual(feat["EAS_ID"], 168)
      self.assertEqual(feat["PRFEDEA"], "35043411")
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          "POLYGON ((479819.84375 4765180.5,479690.1875 4765259.5,479647.0"
          " 4765369.5,479730.375 4765400.5,480039.03125 4765539.5,480035.34375"
          " 4765558.5,480159.78125 4765610.5,480202.28125 4765482.0,480365.0"
          " 4765015.5,480389.6875 4764950.0,480133.96875 4764856.5,480080.28125"
          " 4764979.5,480082.96875 4765049.5,480088.8125 4765139.5,480059.90625"
          " 4765239.5,480019.71875 4765319.5,479980.21875 4765409.5,479909.875"
          " 4765370.0,479859.875 4765270.0,479819.84375 4765180.5))",
      )

  def test_pipeline(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    last_pct = [0.0]

    def my_progress(pct: float, _msg: str, _user_data: object) -> bool:
      last_pct[0] = pct
      return True

    alg = _get_pipeline_alg()
    alg["input"] = [poly_shp, poly_shp]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["pipeline"] = "concat ! write"
    self.assertTrue(alg.Run(my_progress))
    self.assertEqual(last_pct[0], 1.0)
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    self.assertEqual(ds.GetLayerByName("poly").GetFeatureCount(), 20)

  def test_dst_crs(self) -> None:
    srs_4326 = osr.SpatialReference()
    srs_4326.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    srs_4326.ImportFromEPSG(4326)

    srs_32631 = osr.SpatialReference()
    srs_32631.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    srs_32631.ImportFromEPSG(32631)

    mem_drv = gdal.GetDriverByName("MEM")
    ds1 = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr1 = ds1.CreateLayer("test", srs=srs_4326)
    f1 = ogr.Feature(lyr1.GetLayerDefn())
    f1.SetGeometry(ogr.CreateGeometryFromWkt("POINT (2 49)"))
    lyr1.CreateFeature(f1)

    ds2 = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr2 = ds2.CreateLayer("test", srs=srs_32631)
    f2 = ogr.Feature(lyr2.GetLayerDefn())
    f2.SetGeometry(ogr.CreateGeometryFromWkt("POINT (500000 0)"))
    lyr2.CreateFeature(f2)

    for dst_crs in (None, "EPSG:4326"):
      alg = _get_alg()
      alg["input"] = [ds1, ds2]
      alg["output"] = ""
      alg["output-format"] = "MEM"
      if dst_crs is not None:
        alg["dst-crs"] = dst_crs
      self.assertTrue(alg.Run())
      out_ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = out_ds.GetLayerByName("test")
      self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "4326")
      feat1 = lyr.GetNextFeature()
      self.assertIsNotNone(feat1)
      self.assertEqual(feat1.GetGeometryRef().ExportToWkt(), "POINT (2 49)")
      feat2 = lyr.GetNextFeature()
      self.assertIsNotNone(feat2)
      self._assert_geometry_near(feat2, "POINT (3 0)")

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["dst-crs"] = "EPSG:32631"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = out_ds.GetLayerByName("test")
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "32631")
    feat1 = lyr.GetNextFeature()
    self.assertIsNotNone(feat1)
    self._assert_geometry_near(
        feat1, "POINT (426857.9877172817 5427937.523464922)"
    )
    feat2 = lyr.GetNextFeature()
    self.assertIsNotNone(feat2)
    self._assert_geometry_near(feat2, "POINT (500000 0)")

    ds3 = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr3 = ds3.CreateLayer("test")
    f3 = ogr.Feature(lyr3.GetLayerDefn())
    f3.SetGeometry(ogr.CreateGeometryFromWkt("POINT (500000 0)"))
    lyr3.CreateFeature(f3)

    alg = _get_alg()
    alg["input"] = [ds3]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["dst-crs"] = "EPSG:4326"
    with self.assertRaisesRegex(
        RuntimeError,
        "concat: Layer 'test' of '' has no spatial reference system",
    ):
      alg.Run()

    alg = _get_alg()
    alg["input"] = [ds3]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-crs"] = "EPSG:32631"
    alg["dst-crs"] = "EPSG:4326"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = out_ds.GetLayerByName("test")
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "4326")
    feat3 = lyr.GetNextFeature()
    self.assertIsNotNone(feat3)
    self._assert_geometry_near(feat3, "POINT (3 0)")

  def test_input_layer_name(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    idlink_dbf = utilities_util.get_testdata_path("../ogr/data/idlink.dbf")
    alg = _get_alg()
    alg["input"] = [poly_shp, idlink_dbf]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input-layer"] = "idlink"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    self.assertEqual(ds.GetLayer(0).GetName(), "idlink")

  @parameterized.parameters(
      (None, ["a", "b", "c"]),
      ("union", ["a", "b", "c"]),
      ("intersection", ["c"]),
  )
  def test_field_strategy(
      self, strategy: str | None, expected_fields: Sequence[str]
  ) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    ds1 = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr1 = ds1.CreateLayer("test")
    lyr1.CreateField(ogr.FieldDefn("a"))
    lyr1.CreateField(ogr.FieldDefn("c"))

    ds2 = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    lyr2 = ds2.CreateLayer("test")
    lyr2.CreateField(ogr.FieldDefn("b"))
    lyr2.CreateField(ogr.FieldDefn("c"))

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    if strategy is not None:
      alg["field-strategy"] = strategy
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    lyr = ds.GetLayerByName("test")
    got_fields = [
        lyr.GetLayerDefn().GetFieldDefn(i).GetName()
        for i in range(lyr.GetLayerDefn().GetFieldCount())
    ]
    self.assertCountEqual(got_fields, expected_fields)

  def test_mode_single_merge_and_stack(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    # Single mode with distinct layer names.
    ds_a = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    la = ds_a.CreateLayer("test")
    la.CreateField(ogr.FieldDefn("a"))
    la.CreateField(ogr.FieldDefn("c"))
    ds_b = mem_drv.Create("", 0, 0, 0, gdal.GDT_Unknown)
    lb = ds_b.CreateLayer("test2")
    lb.CreateField(ogr.FieldDefn("b"))
    lb.CreateField(ogr.FieldDefn("c"))

    alg = _get_alg()
    alg["input"] = [ds_a, ds_b]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["mode"] = "single"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    lyr = ds.GetLayerByName("merged")
    got_fields = [
        lyr.GetLayerDefn().GetFieldDefn(i).GetName()
        for i in range(lyr.GetLayerDefn().GetFieldCount())
    ]
    self.assertCountEqual(got_fields, ["a", "b", "c"])

    # Multi-layer fixtures for default, stack, and single modes.
    ds1 = mem_drv.Create("ds1", 0, 0, 0, gdal.GDT_Unknown)
    for name in ("my_lyr_name_1", "my_lyr_name_2"):
      l = ds1.CreateLayer(name)
      l.CreateFeature(ogr.Feature(l.GetLayerDefn()))
    ds2 = mem_drv.Create("ds2", 0, 0, 0, gdal.GDT_Unknown)
    for name in ("my_lyr_name_1", "my_lyr_name_2"):
      l = ds2.CreateLayer(name)
      l.CreateFeature(ogr.Feature(l.GetLayerDefn()))

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 2)
    self.assertEqual(
        [ds.GetLayer(i).GetName() for i in range(2)],
        ["my_lyr_name_1", "my_lyr_name_2"],
    )
    for i in range(2):
      self.assertEqual(ds.GetLayer(i).GetFeatureCount(), 2)

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "not_allowed"
    with self.assertRaisesRegex(
        RuntimeError,
        "'layer-name' name argument cannot be specified in"
        " mode=merge-per-layer-name",
    ):
      alg.Run()

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["mode"] = "stack"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 4)
    self.assertEqual(
        [ds.GetLayer(i).GetName() for i in range(4)],
        [
            "ds1_my_lyr_name_1",
            "ds1_my_lyr_name_2",
            "ds2_my_lyr_name_1",
            "ds2_my_lyr_name_2",
        ],
    )
    for i in range(4):
      self.assertEqual(ds.GetLayer(i).GetFeatureCount(), 1)

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["mode"] = "stack"
    alg["output-layer"] = "{DS_INDEX}-{LAYER_INDEX}"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        [ds.GetLayer(i).GetName() for i in range(4)],
        ["0-0", "0-1", "1-0", "1-1"],
    )

    alg = _get_alg()
    alg["input"] = [ds1, ds2]
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["mode"] = "single"
    alg["output-layer"] = "my-output-layer"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    self.assertEqual(ds.GetLayer(0).GetName(), "my-output-layer")
    self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 4)

  def test_stack_from_filesystem_source(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    alg = _get_alg()
    alg["input"] = poly_shp
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["mode"] = "stack"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    self.assertEqual(ds.GetLayer(0).GetName(), "poly")

  @parameterized.parameters(
      (None, "{AUTO_NAME}", "my_ds_name_my_lyr_name"),
      ("src", None, "my_ds_name_my_lyr_name"),
      ("src", "{DS_NAME}", "my_ds_name"),
      ("src", "{DS_BASENAME}", "my_ds_name"),
      ("src", "{DS_INDEX}", "0"),
      ("src", "{LAYER_NAME}", "my_lyr_name"),
      ("src", "{LAYER_INDEX}", "0"),
  )
  def test_source_layer_field(
      self,
      field_name: str | None,
      field_content: str | None,
      expected_val: str,
  ) -> None:
    ds1 = gdal.GetDriverByName("MEM").Create(
        "my_ds_name", 0, 0, 0, gdal.GDT_Unknown
    )
    lyr = ds1.CreateLayer("my_lyr_name")
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

    alg = _get_alg()
    alg["input"] = ds1
    alg["output"] = ""
    alg["output-format"] = "MEM"
    if field_name is not None:
      alg["source-layer-field-name"] = field_name
    if field_content is not None:
      alg["source-layer-field-content"] = field_content
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetLayerCount(), 1)
    out_lyr = ds.GetLayer(0)
    self.assertEqual(out_lyr.GetName(), "my_lyr_name")
    self.assertEqual(out_lyr.GetFeatureCount(), 1)
    feat = out_lyr.GetNextFeature()
    col = field_name if field_name is not None else "source_ds_lyr"
    self.assertEqual(feat[col], expected_val)

  def test_gdalg_streamed(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": (
            f"gdal vector concat {poly_shp} --output-format=stream foo"
        ),
        "relative_paths_relative_to_this_file": False,
    }
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      gdalg_filename = os.path.join(tmpdir, "tmp.gdalg.json")
      with open(gdalg_filename, "w", encoding="utf-8") as f:
        json.dump(spec, f)
      with gdal.OpenEx(gdalg_filename) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)


if __name__ == "__main__":
  absltest.main()
