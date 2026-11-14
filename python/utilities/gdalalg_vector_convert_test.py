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

"""Tests for `gdal vector convert`.

Ported from upstream autotest/utilities/test_gdalalg_vector_convert.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_convert_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["convert"]


def _get_sqlite_version() -> tuple[int, ...]:
  if not utilities_util.has_driver("GPKG"):
    return (0, 0, 0)
  ds = ogr.Open(":memory:")
  sql_lyr = ds.ExecuteSQL("SELECT sqlite_version()")
  feat = sql_lyr.GetNextFeature()
  version = feat.GetField(0)
  ds.ReleaseResultSet(sql_lyr)
  ds = None
  return tuple(int(x) for x in version.split(".")[0:3])


class GdalalgVectorConvertTest(gdaltest.TestCase, parameterized.TestCase):

  def test_base(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_convert_base") as tmp:
      out_filename = f"{tmp}/out.gpkg"

      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize([poly_shp, out_filename])
      )

      with gdal.OpenEx(out_filename, gdal.OF_UPDATE) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        for i in range(10):
          ds.GetLayer(0).DeleteFeature(i + 1)

      with self.assertRaisesRegex(RuntimeError, "already exists"):
        _get_convert_alg().ParseRunAndFinalize([poly_shp, out_filename])

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize(
              ["--overwrite", poly_shp, out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize(
              ["--append", poly_shp, out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 20)

      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize(
              ["--update", "--nln", "layer2", poly_shp, out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayerByName("poly").GetFeatureCount(), 20)
        self.assertEqual(ds.GetLayerByName("layer2").GetFeatureCount(), 10)

      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize([
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

  def test_dsco(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_convert_dsco") as tmp:
      out_filename = f"{tmp}/out.gpkg"
      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize(
              [poly_shp, out_filename, "--co", "ADD_GPKG_OGR_CONTENTS=NO"]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        with ds.ExecuteSQL(
            "SELECT * FROM sqlite_master WHERE name = 'gpkg_ogr_contents'"
        ) as lyr:
          self.assertEqual(lyr.GetFeatureCount(), 0)

  def test_lco(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_convert_lco") as tmp:
      out_filename = f"{tmp}/out.gpkg"
      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize(
              [poly_shp, out_filename, "--lco", "FID=my_fid"]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFIDColumn(), "my_fid")

  def test_progress(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_convert_progress") as tmp:
      out_filename = f"{tmp}/out.shp"
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: object) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      self.assertTrue(
          _get_convert_alg().ParseRunAndFinalize(
              [poly_shp, out_filename], my_progress
          )
      )
      self.assertEqual(last_pct[0], 1.0)
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_wrong_layer_name(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with self.assertRaisesRegex(
        RuntimeError, "Cannot find source layer 'invalid'"
    ):
      _get_convert_alg().ParseRunAndFinalize(
          [poly_shp, "--of=MEM", "--output=empty", "--layer", "invalid"]
      )

  def test_error_output_not_set(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    convert = _get_convert_alg()
    convert["input"] = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    convert["output"] = convert["output"]
    with self.assertRaisesRegex(
        RuntimeError,
        "convert: Argument 'output' has no dataset object or dataset name",
    ):
      convert.Run()

  def test_vsistdout(self) -> None:
    if not utilities_util.has_driver("GeoJSON"):
      self.skipTest("GeoJSON driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("vector_convert_vsistdout") as tmp:
      convert = _get_convert_alg()
      convert["input"] = utilities_util.get_testdata_path(
          "../ogr/data/poly.shp"
      )
      convert["output"] = f"/vsistdout_redirect/{tmp}/tmp.json"
      convert["output-format"] = "GeoJSON"
      self.assertTrue(convert.Run())
      self.assertTrue(convert.Finalize())
      with gdal.OpenEx(f"{tmp}/tmp.json") as ds:
        self.assertIsNotNone(ds)

  def test_overwrite_fgdb(self) -> None:
    if not utilities_util.has_driver("OpenFileGDB"):
      self.skipTest("OpenFileGDB driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vector_convert_fgdb") as tmp:
      out_gdb = f"{tmp}/out.gdb"
      convert = _get_convert_alg()
      convert["input"] = poly_shp
      convert["output"] = out_gdb
      convert["output-format"] = "OpenFileGDB"
      convert["layer-creation-option"] = {
          "TARGET_ARCGIS_VERSION": "ARCGIS_PRO_3_2_OR_LATER"
      }
      self.assertTrue(convert.Run())
      self.assertTrue(convert.Finalize())

      extra_file = f"{out_gdb}/new_file.txt"
      gdal.FileFromMemBuffer(extra_file, "foo")
      self.assertIsNotNone(gdal.VSIStatL(extra_file))

      convert = _get_convert_alg()
      convert["input"] = poly_shp
      convert["output"] = out_gdb
      convert["output-format"] = "OpenFileGDB"
      convert["overwrite"] = True
      convert["layer-creation-option"] = {
          "TARGET_ARCGIS_VERSION": "ARCGIS_PRO_3_2_OR_LATER"
      }
      self.assertTrue(convert.Run())
      self.assertTrue(convert.Finalize())
      self.assertIsNone(gdal.VSIStatL(extra_file))

  def test_overwrite_non_dataset_directory(self) -> None:
    if not utilities_util.has_driver("OpenFileGDB"):
      self.skipTest("OpenFileGDB driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("vector_convert_nondir") as tmp:
      gdal.FileFromMemBuffer(f"{tmp}/out/foo", "bar")
      convert = _get_convert_alg()
      convert["input"] = utilities_util.get_testdata_path(
          "../ogr/data/poly.shp"
      )
      convert["output"] = f"{tmp}/out"
      convert["output-format"] = "OpenFileGDB"
      convert["overwrite"] = True
      with self.assertRaisesRegex(
          RuntimeError,
          "already exists, but is not recognized as a valid GDAL dataset."
          " Please manually delete it before retrying",
      ):
        convert.Run()

  def test_overwrite_non_dataset_file(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("vector_convert_nonfile") as tmp:
      out_gpkg = f"{tmp}/out.gpkg"
      gdal.FileFromMemBuffer(out_gpkg, "bar")
      convert = _get_convert_alg()
      convert["input"] = utilities_util.get_testdata_path(
          "../ogr/data/poly.shp"
      )
      convert["output"] = out_gpkg
      convert["output-format"] = "GPKG"
      convert["overwrite"] = True
      self.assertTrue(convert.Run())
      self.assertTrue(convert.Finalize())

  def test_skip_errors(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = src_ds.CreateLayer("test")
    for wkt in (
        "POINT(1 2)",
        "LINESTRING(1 2, 3 4)",
        "POINT(3 4)",
    ):
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(feat)

    with utilities_util.vsimem_dir("vector_convert_skip_err") as tmp:
      convert = _get_convert_alg()
      convert["input"] = src_ds
      convert["output"] = f"{tmp}/out.shp"
      convert["skip-errors"] = True
      self.assertTrue(convert.Run())
      out_ds = utilities_util.get_arg_dataset(convert, "output")
      self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 2)
      out_ds = None
      self.assertTrue(convert.Finalize())

  def test_to_non_available_db_driver(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    convert = _get_convert_alg()
    convert["input"] = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    convert["output"] = "MongoDBv3:tmp/foo"
    if utilities_util.has_driver("MongoDBv3"):
      with self.assertRaisesRegex(
          RuntimeError, "Unable to open existing output datasource"
      ):
        convert.Run()
    else:
      with self.assertRaisesRegex(
          RuntimeError,
          "Filename MongoDBv3:tmp/foo starts with the connection prefix of"
          " driver MongoDBv3, which is not enabled in this GDAL build. If that"
          " filename is really intended, explicitly specify its output format",
      ):
        convert.Run()

  def test_output_format_not_guessed(self) -> None:
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    with utilities_util.vsimem_dir("vector_convert_noguess") as tmp:
      convert = _get_convert_alg()
      convert["input"] = utilities_util.get_testdata_path(
          "../ogr/data/poly.shp"
      )
      convert["output"] = f"{tmp}/foo"
      with self.assertRaisesRegex(RuntimeError, "Cannot guess driver for"):
        convert.Run()

  def test_to_stdout(self) -> None:
    if not utilities_util.has_driver("GeoJSON"):
      self.skipTest("GeoJSON driver not available")
    if not utilities_util.has_driver("ESRI Shapefile"):
      self.skipTest("ESRI Shapefile driver not available")

    path_shp = utilities_util.get_testdata_path("data/path.shp")
    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "convert", "--of=GeoJSON", path_shp, "/vsistdout/"]
    )
    self.assertEqual(code, 0)
    self.assertFalse(out.startswith("0...10...20..."))
    with ogr.Open(out) as ds:
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  @parameterized.parameters("GPKG", "SQLite")
  def test_upsert(self, output_format: str) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    if not utilities_util.has_driver(output_format):
      self.skipTest(f"{output_format} driver not available")
    if _get_sqlite_version() < (3, 24, 0):
      self.skipTest("sqlite >= 3.24 needed")

    with utilities_util.vsimem_dir(
        f"vector_convert_upsert_{output_format.lower()}"
    ) as tmp:
      filename = f"{tmp}/upsert_without_fid.{output_format.lower()}"

      ds = gdal.GetDriverByName(output_format).Create(
          filename, 0, 0, 0, gdal.GDT_Unknown
      )
      lyr = ds.CreateLayer("foo")
      self.assertEqual(
          lyr.CreateField(ogr.FieldDefn("other", ogr.OFTString)), 0
      )
      unique_field = ogr.FieldDefn("unique_field", ogr.OFTString)
      unique_field.SetUnique(True)
      self.assertEqual(lyr.CreateField(unique_field), 0)
      for i in range(5):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField("unique_field", i + 1)
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(f"POINT ({i} {i})"))
        self.assertEqual(lyr.CreateFeature(feat), 0)
      ds = None

      def create_src_file() -> gdal.Dataset:
        src_filename = f"{tmp}/upsert_src.gpkg"
        src_ds = gdal.GetDriverByName("GPKG").Create(
            src_filename, 0, 0, 0, gdal.GDT_Unknown
        )
        src_lyr = src_ds.CreateLayer("foo")
        self.assertEqual(
            src_lyr.CreateField(ogr.FieldDefn("other", ogr.OFTString)), 0
        )
        src_unique = ogr.FieldDefn("unique_field", ogr.OFTString)
        src_unique.SetUnique(True)
        self.assertEqual(src_lyr.CreateField(src_unique), 0)
        feat = ogr.Feature(src_lyr.GetLayerDefn())
        feat.SetField("unique_field", "2")
        feat.SetField("other", "foo")
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt("POINT (10 10)"))
        src_lyr.CreateFeature(feat)
        return src_ds

      if output_format == "SQLite":
        with self.assertRaisesRegex(
            RuntimeError, "SQLite driver doest not support upsert"
        ):
          gdal.Run(
              "vector",
              "convert",
              input=create_src_file(),
              output=filename,
              upsert=True,
          )
      else:
        gdal.Run(
            "vector",
            "convert",
            input=create_src_file(),
            output=filename,
            upsert=True,
        )
        with ogr.Open(filename) as out_ds:
          out_lyr = out_ds.GetLayer(0)
          feat = out_lyr.GetFeature(2)
          self.assertEqual(feat["unique_field"], "2")
          self.assertEqual(feat["other"], "foo")
          self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (10 10)")


if __name__ == "__main__":
  absltest.main()
