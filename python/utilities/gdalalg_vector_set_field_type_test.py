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

"""Tests for `gdal vector set-field-type`.

Ported from upstream autotest/utilities/test_gdalalg_vector_set_field_type.py.

Upstream copyright:
  Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
  SPDX-License-Identifier: MIT
"""

import os
from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_set_field_type_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["set-field-type"]


class GdalalgVectorSetFieldTypeTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      (ogr.OFTString, ogr.OFSTNone, "Integer", "123", 123),
      (ogr.OFTBinary, ogr.OFSTNone, "String", b"foo", "666F6F"),
      (ogr.OFTString, ogr.OFSTNone, "Real", "123.456", 123.456),
      (ogr.OFTString, ogr.OFSTNone, "String", "foo", "foo"),
      (ogr.OFTInteger, ogr.OFSTNone, "String", 123, "123"),
      (ogr.OFTInteger, ogr.OFSTNone, "Real", 123, 123.0),
      (ogr.OFTInteger, ogr.OFSTNone, "Integer", 123, 123),
      (ogr.OFTReal, ogr.OFSTNone, "String", 123.456, "123.456"),
      (ogr.OFTReal, ogr.OFSTNone, "Integer", 123.456, 123),
      (ogr.OFTReal, ogr.OFSTNone, "Real", 123.456, 123.456),
      (ogr.OFTString, ogr.OFSTNone, "Date", "2024/06/01", "2024/06/01"),
      (
          ogr.OFTString,
          ogr.OFSTNone,
          "DateTime",
          "2024/06/01 12:34:56",
          "2024/06/01 12:34:56",
      ),
      (ogr.OFTString, ogr.OFSTNone, "Time", "12:34:56", "12:34:56"),
      (
          ogr.OFTString,
          ogr.OFSTNone,
          "Integer64",
          "1234567890123",
          1234567890123,
      ),
      (
          ogr.OFTString,
          ogr.OFSTJSON,
          "StringList",
          '["foo","bar","baz"]',
          ["foo", "bar", "baz"],
      ),
      (ogr.OFTString, ogr.OFSTJSON, "IntegerList", "[1,2,3]", [1, 2, 3]),
      (
          ogr.OFTString,
          ogr.OFSTJSON,
          "RealList",
          "[1.1,2.2,3.3]",
          [1.1, 2.2, 3.3],
      ),
      (
          ogr.OFTString,
          ogr.OFSTJSON,
          "Integer64List",
          "[1234567890123,2345678901234]",
          [1234567890123, 2345678901234],
      ),
      (
          ogr.OFTStringList,
          ogr.OFSTNone,
          "String",
          '["foo","bar","baz"]',
          "(3:foo,bar,baz)",
      ),
      (ogr.OFTIntegerList, ogr.OFSTNone, "String", "[1,2,3]", "(3:1,2,3)"),
      (
          ogr.OFTRealList,
          ogr.OFSTNone,
          "String",
          "[1.1,2.2,3.3]",
          "(3:1.1,2.2,3.3)",
      ),
      (
          ogr.OFTStringList,
          ogr.OFSTNone,
          "JSON",
          '["foo","bar","baz"]',
          '[ "foo", "bar", "baz" ]',
      ),
      (
          ogr.OFTString,
          ogr.OFSTNone,
          "UUID",
          "550e8400-e29b-41d4-a716-446655440000",
          "550e8400-e29b-41d4-a716-446655440000",
      ),
      (ogr.OFTString, ogr.OFSTNone, "boolean", "true", True),
      (ogr.OFTString, ogr.OFSTNone, "float32", "1.5", 1.5),
      (ogr.OFTString, ogr.OFSTNone, "int16", "123", 123),
      (ogr.OFTInteger, ogr.OFSTInt16, "integer", -32768, -32768),
      (ogr.OFTInteger, ogr.OFSTBoolean, "integer", True, 1),
      (ogr.OFTInteger, ogr.OFSTBoolean, "integer", False, 0),
  )
  def test_set_field_type(
      self,
      src_type: int,
      src_subtype: int,
      dest_type_str: str,
      src_value: Any,
      expected_value: Any,
  ) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateDataSource("")
    lyr = src_ds.CreateLayer("layer", geom_type=ogr.wkbPoint)
    fld_defn = ogr.FieldDefn("value", src_type)
    fld_defn.SetSubType(src_subtype)
    lyr.CreateField(fld_defn)
    lyr.CreateField(ogr.FieldDefn("new_type", ogr.OFTString))

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT(1 2)"))
    feat["value"] = src_value
    feat["new_type"] = dest_type_str
    lyr.CreateFeature(feat)

    alg = _get_set_field_type_alg()
    alg["input"] = src_ds
    self.assertTrue(
        alg.ParseCommandLineArguments([
            "--field-name",
            "value",
            "--field-type",
            dest_type_str,
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    out_field_defn = out_lyr.GetLayerDefn().GetFieldDefn(0)
    expected_type = ogr.GetFieldTypeByName(dest_type_str)

    if expected_type == ogr.OFTString and dest_type_str.lower() != "string":
      expected_subtype = ogr.GetFieldSubtypeByName(dest_type_str)
      self.assertEqual(out_field_defn.GetSubType(), expected_subtype)
    else:
      self.assertEqual(out_field_defn.GetType(), expected_type)
      self.assertEqual(out_field_defn.GetSubType(), ogr.OFSTNone)

    self.assertEqual(out_f["value"], expected_value)
    self.assertEqual(out_f["new_type"], dest_type_str)

    geom = out_f.GetGeometryRef()
    self.assertIsNotNone(geom)
    assert geom is not None
    self.assertEqual(geom.ExportToWkt(), "POINT (1 2)")

  def test_errors(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateDataSource("")
    lyr = src_ds.CreateLayer("layer", geom_type=ogr.wkbNone)
    lyr.CreateField(ogr.FieldDefn("value", ogr.OFTString))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["value"] = "foo"
    lyr.CreateFeature(feat)

    alg = _get_set_field_type_alg()
    alg["input"] = src_ds
    with self.assertRaisesRegex(
        RuntimeError, "Cannot find field 'non_existing_field' in layer 'layer'"
    ):
      alg.ParseCommandLineArguments([
          "--field-name",
          "non_existing_field",
          "--field-type",
          "Integer",
          "--of",
          "MEM",
          "--output",
          "memory_ds",
      ])

    alg = _get_set_field_type_alg()
    alg["input"] = src_ds
    with self.assertRaisesRegex(
        RuntimeError,
        "set-field-type: Invalid value for argument 'field-type':"
        " 'NonExistingType'",
    ):
      alg.ParseCommandLineArguments([
          "--field-name",
          "value",
          "--field-type",
          "NonExistingType",
          "--of",
          "MEM",
          "--output",
          "memory_ds",
      ])

    alg = _get_set_field_type_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Positional arguments starting at 'INPUT' have not been specified.",
    ):
      alg.ParseCommandLineArguments([
          "--field-name",
          "value",
          "--field-type",
          "Integer",
          "--of",
          "MEM",
          "--output",
          "memory_ds",
      ])

    alg = _get_set_field_type_alg()
    alg["input"] = src_ds
    with self.assertRaisesRegex(
        RuntimeError,
        "Positional arguments starting at 'OUTPUT' have not been specified.",
    ):
      alg.ParseCommandLineArguments([
          "--field-name",
          "value",
          "--field-type",
          "Integer",
          "--of",
          "MEM",
      ])

  def test_in_pipeline(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gdal.Run(
        "pipeline",
        pipeline=(
            f"read {poly_shp} ! set-field-type --field-name EAS_ID"
            " --field-type Integer ! write --output-format stream stream"
        ),
    ) as alg:
      ds = alg.Output()
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat["EAS_ID"], 168)

    with self.assertRaisesRegex(
        RuntimeError, "Cannot find field 'invalid_field' in layer 'poly'"
    ):
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {poly_shp} ! set-field-type --field-name invalid_field"
              " --field-type Integer ! write --output-format stream stream"
          ),
      )

  def test_completion(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")

    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "vector", "set-field-type", "--field-type"]
    )
    self.assertEqual(code, 0)
    self.assertIn("Binary", out)

    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "vector",
        "set-field-type",
        "--input",
        poly_shp,
        "--field-name",
    ])
    self.assertEqual(code, 0)
    self.assertIn("EAS_ID", out)

    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "pipeline",
        "read",
        poly_shp,
        "!",
        "set-field-type",
        "--field-name",
    ])
    self.assertEqual(code, 0)
    self.assertIn("EAS_ID", out)

    out, _, _ = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "pipeline",
        "read",
        poly_shp,
        "!",
        "tee",
        "[",
        "write",
        "/vsimem/out.gpkg",
        "]",
        "!",
        "set-field-type",
        "--field-name",
    ])
    self.assertNotIn("EAS_ID", out)

    out, _, _ = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "pipeline",
        "read",
        byte_tif,
        "!",
        "contour",
        "--interval",
        "10",
        "!",
        "set-field-type",
        "--field-name",
    ])
    self.assertNotIn("EAS_ID", out)

  def test_multiple_layers(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with (
        utilities_util.vsimem_dir("set_field_type_multi_in") as tmp_vsimem,
        gcore_util.TestTemporaryDirectory() as tmpdir,
    ):
      in_filename = f"{tmp_vsimem}/in.gpkg"
      out_filename = os.path.join(tmpdir, "out.gpkg")
      src_ds = gdal.GetDriverByName("GPKG").CreateDataSource(in_filename)

      fld_defn = ogr.FieldDefn("test_field", ogr.OFTString)
      for name, val in (("layer1", "123"), ("layer2", "456")):
        lyr = src_ds.CreateLayer(name, geom_type=ogr.wkbNone)
        lyr.CreateField(fld_defn)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat["test_field"] = val
        lyr.CreateFeature(feat)
        lyr = None

      alg = _get_set_field_type_alg()
      alg["input"] = src_ds
      self.assertTrue(
          alg.ParseCommandLineArguments([
              "--field-name",
              "test_field",
              "--field-type",
              "Integer",
              "--output",
              out_filename,
          ])
      )
      self.assertTrue(alg.Run())

      with gdal.OpenEx(out_filename, gdal.OF_VECTOR) as out_ds:
        self.assertEqual(out_ds.GetLayerCount(), 2)
        for idx, expected_val in ((0, 123), (1, 456)):
          lyr = out_ds.GetLayer(idx)
          self.assertEqual(lyr.GetFeatureCount(), 1)
          fdefn = lyr.GetLayerDefn().GetFieldDefn(0)
          self.assertEqual(fdefn.GetType(), ogr.OFTInteger)
          self.assertEqual(fdefn.GetSubType(), ogr.OFSTNone)
          self.assertEqual(lyr.GetNextFeature()["test_field"], expected_val)

      alg = _get_set_field_type_alg()
      alg["input"] = src_ds
      self.assertTrue(
          alg.ParseCommandLineArguments([
              "--overwrite",
              "--field-name",
              "test_field",
              "--field-type",
              "Integer",
              "--layer",
              "layer2",
              "--output",
              out_filename,
          ])
      )
      self.assertTrue(alg.Run())
      src_ds = None

      with gdal.OpenEx(out_filename, gdal.OF_VECTOR) as out_ds:
        self.assertEqual(out_ds.GetLayerCount(), 1)
        lyr = out_ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1)
        fdefn = lyr.GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(fdefn.GetType(), ogr.OFTInteger)
        self.assertEqual(fdefn.GetSubType(), ogr.OFSTNone)
        self.assertEqual(lyr.GetNextFeature()["test_field"], 456)

  def test_src_field_type(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateDataSource("")
    lyr = src_ds.CreateLayer("layer", geom_type=ogr.wkbPoint)
    lyr.CreateField(ogr.FieldDefn("string1", ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn("string2", ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn("real", ogr.OFTReal))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["string1"] = "123"
    feat["string2"] = "456"
    feat["real"] = 1.25
    lyr.CreateFeature(feat)

    with gdal.Run(
        "vector",
        "set-field-type",
        input=src_ds,
        src_field_type="String",
        dst_field_type="Integer",
        output_format="MEM",
    ) as alg:
      ds = alg.Output()
      out_f = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(out_f["string1"], 123)
      self.assertEqual(out_f["string2"], 456)
      self.assertEqual(out_f["real"], 1.25)

  def test_pipeline_fuse_in_open(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    with utilities_util.vsimem_dir("set_field_type_csv") as tmp:
      csv_path = f"{tmp}/test.csv"
      csvt_path = f"{tmp}/test.csvt"
      with gdal.VSIFile(csv_path, "wb") as f:
        f.write(b"str,int,int2\n2025-09-30,1,2\n")
      with gdal.VSIFile(csvt_path, "wb") as f:
        f.write(b"String,Integer,Integer\n")

      with gdal.Run(
          "pipeline",
          pipeline=(
              f"read {csv_path} ! set-field-type --src-field-type String"
              " --field-type Date ! set-field-type --field-name int"
              " --field-type Integer64 ! write --format MEM --output whatever"
          ),
      ) as alg:
        ds = alg.Output()
        lyr = ds.GetLayer(0)
        lyr_defn = lyr.GetLayerDefn()
        self.assertEqual(lyr_defn.GetFieldDefn(0).GetType(), ogr.OFTDate)
        self.assertEqual(lyr_defn.GetFieldDefn(1).GetType(), ogr.OFTInteger64)
        self.assertEqual(lyr_defn.GetFieldDefn(2).GetType(), ogr.OFTInteger)

  def test_pipeline_fuse_in_open_not_supported(self) -> None:
    with utilities_util.vsimem_dir("set_field_type_dbf") as tmp:
      dbf_path = f"{tmp}/test.dbf"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(dbf_path) as ds:
        lyr = ds.CreateLayer("test", geom_type=ogr.wkbNone)
        lyr.CreateField(ogr.FieldDefn("str", ogr.OFTString))
        lyr.CreateField(ogr.FieldDefn("int", ogr.OFTInteger))
        lyr.CreateField(ogr.FieldDefn("int2", ogr.OFTInteger))

      with gdal.Run(
          "pipeline",
          pipeline=(
              f"read {dbf_path} ! set-field-type --src-field-type String"
              " --field-type Date ! set-field-type --field-name int"
              " --field-type Integer64 ! write --format MEM --output whatever"
          ),
      ) as alg:
        ds = alg.Output()
        lyr = ds.GetLayer(0)
        lyr_defn = lyr.GetLayerDefn()
        self.assertEqual(lyr_defn.GetFieldDefn(0).GetType(), ogr.OFTDate)
        self.assertEqual(lyr_defn.GetFieldDefn(1).GetType(), ogr.OFTInteger64)
        self.assertEqual(lyr_defn.GetFieldDefn(2).GetType(), ogr.OFTInteger)


if __name__ == "__main__":
  absltest.main()
