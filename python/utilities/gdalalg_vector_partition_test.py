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

"""Tests for `gdal vector partition`.

Ported from upstream autotest/utilities/test_gdalalg_vector_partition.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _make_src_ds(unique_constraint: bool = False) -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").CreateVector("")
  lyr = ds.CreateLayer("test", geom_type=ogr.wkbPoint, options=["FID=my_fid"])
  str_field = ogr.FieldDefn("str_field")
  if unique_constraint:
    str_field.SetUnique(True)
  lyr.CreateField(str_field)
  lyr.CreateField(ogr.FieldDefn("int_field", ogr.OFTInteger))
  lyr.CreateField(ogr.FieldDefn("int64 field", ogr.OFTInteger64))
  lyr.CreateField(ogr.FieldDefn("real_field", ogr.OFTReal))

  f = ogr.Feature(lyr.GetLayerDefn())
  f["str_field"] = "one"
  f["int_field"] = 1
  f["int64 field"] = 1234567890123
  f["real_field"] = 1.5
  f.SetFID(10)
  f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
  lyr.CreateFeature(f)

  f = ogr.Feature(lyr.GetLayerDefn())
  f["str_field"] = "one"
  f["int_field"] = 2
  f.SetFID(11)
  lyr.CreateFeature(f)

  f = ogr.Feature(lyr.GetLayerDefn())
  f["str_field"] = "two"
  f["int_field"] = 1
  f.SetFID(12)
  lyr.CreateFeature(f)

  non_spatial = ds.CreateLayer("non spatial", geom_type=ogr.wkbNone)
  non_spatial.CreateField(ogr.FieldDefn("str_field"))
  non_spatial.CreateField(ogr.FieldDefn("int_field", ogr.OFTInteger))
  non_spatial.CreateField(ogr.FieldDefn("int64 field", ogr.OFTInteger64))
  non_spatial.CreateField(ogr.FieldDefn("real_field", ogr.OFTReal))
  non_spatial.CreateFeature(ogr.Feature(non_spatial.GetLayerDefn()))

  return ds


class GdalalgVectorPartitionTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(100, 1)
  def test_str_field(self, max_cache_size: int) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    last_pct = [0.0]

    def on_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, last_pct[0])
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("partition_str") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="GPKG",
          field="str_field",
          max_cache_size=max_cache_size,
          progress=on_progress,
      )
      self.assertEqual(last_pct[0], 1.0)

      with ogr.Open(f"{tmp}/out/test/str_field=one/part_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFIDColumn(), "my_fid")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetFID(), 10)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")

        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 2)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(f"{tmp}/out/test/str_field=two/part_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "two")
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(
          f"{tmp}/out/non%20spatial/"
          "str_field=__HIVE_DEFAULT_PARTITION__/part_0000000001.gpkg"
      ) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  @parameterized.parameters(100, 1)
  def test_int_field(self, transaction_size: int) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_int") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="GPKG",
          field="int_field",
          transaction_size=transaction_size,
      )

      with ogr.Open(f"{tmp}/out/test/int_field=1/part_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFIDColumn(), "my_fid")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetFID(), 10)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")

        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "two")
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(f"{tmp}/out/test/int_field=2/part_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 2)
        self.assertIsNone(lyr.GetNextFeature())

  def test_int64_field(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_int64") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="GPKG",
          field="int64 field",
      )

      with ogr.Open(
          f"{tmp}/out/test/int64%20field=1234567890123/part_0000000001.gpkg"
      ) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFIDColumn(), "my_fid")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetFID(), 10)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(
          f"{tmp}/out/test/int64%20field=__HIVE_DEFAULT_PARTITION__/"
          "part_0000000001.gpkg"
      ) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 2)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "two")
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

  def test_omit_partitioned_field(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_omit") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="GPKG",
          field="str_field",
          omit_partitioned_field=True,
      )

      with ogr.Open(f"{tmp}/out/test/str_field=one/part_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFIDColumn(), "my_fid")
        self.assertEqual(lyr.GetLayerDefn().GetFieldIndex("str_field"), -1)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetFID(), 10)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")

        feat = lyr.GetNextFeature()
        self.assertEqual(feat["int_field"], 2)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(f"{tmp}/out/test/str_field=two/part_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldIndex("str_field"), -1)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

  def test_overwrite_and_append(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_ow") as tmp:
      out_dir = f"{tmp}/out"
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
      )

      with self.assertRaisesRegex(
          RuntimeError, "already exists. Specify --overwrite or --append"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=out_dir,
            format="GPKG",
            field="str_field",
        )

      for _ in range(2):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=out_dir,
            format="GPKG",
            field="str_field",
            overwrite=True,
        )
        with ogr.Open(
            f"{out_dir}/test/str_field=one/part_0000000001.gpkg"
        ) as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 2)

      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
          append=True,
      )
      with ogr.Open(f"{out_dir}/test/str_field=one/part_0000000001.gpkg") as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 4)

      non_part = f"{tmp}/out_not_a_partition_dir"
      gdal.Mkdir(non_part, 0o755)
      gdal.Mkdir(f"{non_part}/foo", 0o755)
      with self.assertRaisesRegex(
          RuntimeError,
          "it does not look like a directory generated by this utility",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=non_part,
            format="GPKG",
            field="str_field",
            overwrite=True,
        )

  def test_errors(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_errors") as tmp:
      with self.assertRaisesRegex(
          RuntimeError,
          "Cannot infer output format. Please specify 'output-format' argument",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            field="str_field",
        )

      with self.assertRaisesRegex(
          RuntimeError, "Output driver has no known file extension"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="MEM",
            field="str_field",
        )

      with self.assertRaisesRegex(
          RuntimeError, "Cannot find field 'non_existing_field' in layer 'test'"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out_non_existing_field",
            format="GPKG",
            field="non_existing_field",
        )

      with self.assertRaisesRegex(
          RuntimeError,
          "Field 'real_field' not valid for partitioning. Only fields of"
          " type String, Integer or Integer64 are accepted",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="GPKG",
            field="real_field",
        )

      with self.assertRaisesRegex(
          RuntimeError, "Cannot create directory '/i_do/not_exits/out'"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output="/i_do/not_exits/out",
            format="GPKG",
            field="str_field",
        )

      with self.assertRaisesRegex(
          RuntimeError, "may not contain special characters or spaces"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out_cannot_create_layer",
            format="GPKG",
            field="str_field",
            layer_creation_option={"FID": "!!invalid!!"},
        )

      with self.assertRaisesRegex(
          RuntimeError, "Invalid value for max-file-size"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="GPKG",
            field="str_field",
            max_file_size=-1,
        )

      with self.assertRaisesRegex(
          RuntimeError, "max-file-size should be at least one MB"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="GPKG",
            field="str_field",
            max_file_size=1,
        )

      dup_ds = gdal.GetDriverByName("MEM").CreateVector("")
      lyr = dup_ds.CreateLayer("test", geom_type=ogr.wkbPoint)
      lyr.CreateField(ogr.FieldDefn("str_field", ogr.OFTString))
      lyr.CreateField(ogr.FieldDefn("dup_field", ogr.OFTString))
      lyr.CreateField(ogr.FieldDefn("dup_field", ogr.OFTString))
      lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
      with self.assertRaisesRegex(
          RuntimeError, "A field with the same name already exists"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=dup_ds,
            output=f"{tmp}/out_cannot_create_field",
            format="GPKG",
            field="str_field",
        )

      multi_geom_ds = gdal.GetDriverByName("MEM").CreateVector("")
      lyr = multi_geom_ds.CreateLayer("test", geom_type=ogr.wkbPoint)
      lyr.CreateField(ogr.FieldDefn("str_field", ogr.OFTString))
      lyr.CreateGeomField(ogr.GeomFieldDefn("geom1", ogr.wkbPoint))
      lyr.CreateGeomField(ogr.GeomFieldDefn("geom2", ogr.wkbPoint))
      lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
      with self.assertRaisesRegex(
          RuntimeError,
          "Cannot create more than one geometry field in GeoPackage",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=multi_geom_ds,
            output=f"{tmp}/out_cannot_create_geom_field",
            format="GPKG",
            field="str_field",
        )

  def test_errors_sxf(self) -> None:
    if not utilities_util.has_driver("SXF"):
      self.skipTest("SXF driver not available")
    with utilities_util.vsimem_dir("partition_sxf") as tmp:
      with self.assertRaisesRegex(
          RuntimeError,
          "Invalid value for argument 'output-format'. Driver 'SXF' does not"
          " expose the required 'DCAP_CREATE' capability",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="SXF",
            field="str_field",
        )

  def test_errors_jml(self) -> None:
    if not utilities_util.has_driver("JML"):
      self.skipTest("JML driver not available")
    with utilities_util.vsimem_dir("partition_jml") as tmp:
      with self.assertRaisesRegex(
          RuntimeError, "Driver 'JML' does not support update"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="JML",
            field="str_field",
            append=True,
        )

  def test_parquet(self) -> None:
    if not utilities_util.has_driver("Parquet"):
      self.skipTest("Parquet driver not available")
    with utilities_util.vsimem_dir("partition_parquet") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="Parquet",
          field="str_field",
      )

      with ogr.Open(
          f"{tmp}/out/test/str_field=one/part_0000000001.parquet"
      ) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldIndex("str_field"), -1)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["int_field"], 2)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(
          f"{tmp}/out/test/str_field=two/part_0000000001.parquet"
      ) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldIndex("str_field"), -1)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

      drv = gdal.GetDriverByName("Parquet")
      if drv is not None and drv.GetMetadataItem("ARROW_DATASET") is not None:
        with ogr.Open(f"PARQUET:{tmp}/out/test") as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 3)

  @parameterized.parameters(100, 1)
  def test_parquet_two_fields(self, max_cache_size: int) -> None:
    if not utilities_util.has_driver("Parquet"):
      self.skipTest("Parquet driver not available")
    with utilities_util.vsimem_dir("partition_parquet_two") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="Parquet",
          field=["str_field", "int_field"],
          max_cache_size=max_cache_size,
      )

      for subpath in (
          "str_field=one/int_field=1",
          "str_field=one/int_field=2",
          "str_field=two/int_field=1",
      ):
        with ogr.Open(
            f"{tmp}/out/test/{subpath}/part_0000000001.parquet"
        ) as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_flatgeobuf(self) -> None:
    if not utilities_util.has_driver("FlatGeoBuf"):
      self.skipTest("FlatGeoBuf driver not available")
    with utilities_util.vsimem_dir("partition_fgb") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=f"{tmp}/out",
          format="FlatGeoBuf",
          field="str_field",
          layer_creation_option=["SPATIAL_INDEX=NO"],
      )

      with ogr.Open(f"{tmp}/out/test/str_field=one/part_0000000001.fgb") as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 2)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(f"{tmp}/out/test/str_field=two/part_0000000001.fgb") as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "two")
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

  def test_feature_limit(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_limit") as tmp:
      out_dir = f"{tmp}/out"
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
          feature_limit=1,
      )

      for subpath in (
          "str_field=one/part_0000000001.gpkg",
          "str_field=one/part_0000000002.gpkg",
          "str_field=two/part_0000000001.gpkg",
      ):
        with ogr.Open(f"{out_dir}/test/{subpath}") as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
          feature_limit=1,
          append=True,
      )

      for subpath in (
          "str_field=one/part_0000000001.gpkg",
          "str_field=one/part_0000000002.gpkg",
          "str_field=one/part_0000000003.gpkg",
          "str_field=one/part_0000000004.gpkg",
          "str_field=two/part_0000000001.gpkg",
          "str_field=two/part_0000000002.gpkg",
      ):
        with ogr.Open(f"{out_dir}/test/{subpath}") as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_max_file_size_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = src_ds.CreateLayer(
        "test", geom_type=ogr.wkbPoint, options=["FID=my_fid"]
    )
    lyr.CreateField(ogr.FieldDefn("str_field"))
    lyr.CreateField(ogr.FieldDefn("int_field", ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn("int64 field", ogr.OFTInteger64))
    lyr.CreateField(ogr.FieldDefn("real_field", ogr.OFTReal))
    lyr.CreateField(ogr.FieldDefn("binary_field", ogr.OFTBinary))
    lyr.CreateField(ogr.FieldDefn("time_field", ogr.OFTTime))
    lyr.CreateField(ogr.FieldDefn("date_field", ogr.OFTDate))
    lyr.CreateField(ogr.FieldDefn("datetime_field", ogr.OFTDateTime))

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["str_field"] = "one"
    feat["int_field"] = 1
    feat["int64 field"] = 1234567890123
    feat["real_field"] = 1.5
    feat["binary_field"] = b"\x01"
    feat["time_field"] = "12:34:56.789"
    feat["date_field"] = "2025-09-02"
    feat["datetime_field"] = "2025-09-02T12:34:56.789+05:30"
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
    for _ in range(5000):
      feat.SetFID(-1)
      lyr.CreateFeature(feat)

    with utilities_util.vsimem_dir("partition_maxsize_gpkg") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=src_ds,
          output=f"{tmp}/out",
          format="GPKG",
          field="str_field",
          max_file_size="1 MB",
      )

      part1 = f"{tmp}/out/test/str_field=one/part_0000000001.gpkg"
      part2 = f"{tmp}/out/test/str_field=one/part_0000000002.gpkg"
      file_size = gdal.VSIStatL(part1).size
      self.assertGreater(file_size, (1024 * 1024) * 0.8)
      self.assertLess(file_size, (1024 * 1024) * 1.1)

      feature_count = 0
      for path in (part1, part2):
        with ogr.Open(path) as ds:
          feature_count += ds.GetLayer(0).GetFeatureCount()
      self.assertEqual(feature_count, 5000)

  def test_max_file_size_sqlite(self) -> None:
    if not utilities_util.has_driver("SQLite"):
      self.skipTest("SQLite driver not available")
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = src_ds.CreateLayer(
        "test", geom_type=ogr.wkbPoint, options=["FID=my_fid"]
    )
    lyr.CreateField(ogr.FieldDefn("str_field"))
    lyr.CreateField(ogr.FieldDefn("int_field", ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn("int64 field", ogr.OFTInteger64))
    lyr.CreateField(ogr.FieldDefn("real_field", ogr.OFTReal))
    lyr.CreateField(ogr.FieldDefn("binary_field", ogr.OFTBinary))
    lyr.CreateField(ogr.FieldDefn("time_field", ogr.OFTTime))
    lyr.CreateField(ogr.FieldDefn("date_field", ogr.OFTDate))
    lyr.CreateField(ogr.FieldDefn("datetime_field", ogr.OFTDateTime))
    lyr.CreateField(ogr.FieldDefn("strlist_field", ogr.OFTStringList))
    lyr.CreateField(ogr.FieldDefn("intlist_field", ogr.OFTIntegerList))
    lyr.CreateField(ogr.FieldDefn("int64list_field", ogr.OFTInteger64List))
    lyr.CreateField(ogr.FieldDefn("reallist_field", ogr.OFTRealList))

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["str_field"] = "one"
    feat["int_field"] = 1
    feat["int64 field"] = 1234567890123
    feat["real_field"] = 1.5
    feat["binary_field"] = b"\x01"
    feat["time_field"] = "12:34:56.789"
    feat["date_field"] = "2025-09-02"
    feat["datetime_field"] = "2025-09-02T12:34:56.789+05:30"
    feat["strlist_field"] = ["foo", "bar"]
    feat["intlist_field"] = [1]
    feat["int64list_field"] = [1234567890123]
    feat["reallist_field"] = [1.5]
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
    for _ in range(5000):
      feat.SetFID(-1)
      lyr.CreateFeature(feat)

    with utilities_util.vsimem_dir("partition_maxsize_sqlite") as tmp:
      gdal.Run(
          "vector",
          "partition",
          input=src_ds,
          output=f"{tmp}/out",
          format="SQLite",
          field="str_field",
          max_file_size="1 MB",
      )

      part1 = f"{tmp}/out/test/str_field=one/part_0000000001.sqlite"
      part2 = f"{tmp}/out/test/str_field=one/part_0000000002.sqlite"
      file_size = gdal.VSIStatL(part1).size
      self.assertGreater(file_size, (1024 * 1024) * 0.6)
      self.assertLess(file_size, (1024 * 1024) * 1.1)

      feature_count = 0
      for path in (part1, part2):
        with ogr.Open(path) as ds:
          feature_count += ds.GetLayer(0).GetFeatureCount()
      self.assertEqual(feature_count, 5000)

  def test_append_error_reopening_file(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_append_reopen") as tmp:
      out_dir = f"{tmp}/out"
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
      )
      with gdal.VSIFile(
          f"{out_dir}/test/str_field=one/part_0000000001.gpkg", "wb"
      ) as f:
        f.write(b"invalid")

      with self.assertRaisesRegex(
          RuntimeError, "not recognized as being in a supported file format"
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=out_dir,
            format="GPKG",
            field="str_field",
            append=True,
        )

  def test_append_error_no_layer(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_append_nolyr") as tmp:
      out_dir = f"{tmp}/out"
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
      )
      with ogr.Open(
          f"{out_dir}/test/str_field=one/part_0000000001.gpkg", update=1
      ) as ds:
        ds.DeleteLayer(0)

      with self.assertRaisesRegex(RuntimeError, "No layer in"):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=out_dir,
            format="GPKG",
            field="str_field",
            append=True,
        )

  def test_append_error_incompatible_schema(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_append_schema") as tmp:
      out_dir = f"{tmp}/out"
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
      )
      with ogr.Open(
          f"{out_dir}/test/str_field=one/part_0000000001.gpkg", update=1
      ) as ds:
        ds.GetLayer(0).DeleteField(0)

      with self.assertRaisesRegex(
          RuntimeError,
          "does not have the same feature definition as the source layer",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=out_dir,
            format="GPKG",
            field="str_field",
            append=True,
        )

  def test_error_cannot_insert_feature(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_err_insert") as tmp:
      with self.assertRaisesRegex(RuntimeError, "Cannot insert feature 11"):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(unique_constraint=True),
            output=f"{tmp}/out",
            format="GPKG",
            field="str_field",
        )

  def test_skip_errors(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_skip_err") as tmp:
      with gdal.quiet_errors():
        with self.assertRaisesRegex(
            RuntimeError, "Transaction not established"
        ):
          gdal.Run(
              "vector",
              "partition",
              input=_make_src_ds(unique_constraint=True),
              output=f"{tmp}/out",
              format="GPKG",
              field="str_field",
              skip_errors=True,
          )

  def test_interrupted_by_user(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    def cancel_progress(pct: float, msg: str, user_data: Any) -> bool:
      del pct, msg, user_data
      return False

    with utilities_util.vsimem_dir("partition_interrupt") as tmp:
      with self.assertRaisesRegex(RuntimeError, "Interrupted by user"):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=f"{tmp}/out",
            format="GPKG",
            field="str_field",
            progress=cancel_progress,
        )

  def test_preexisting_filter(self) -> None:
    if not utilities_util.has_driver("Parquet"):
      self.skipTest("Parquet driver not available")
    with utilities_util.vsimem_dir("partition_filter") as tmp:
      gdal.Run(
          "vector",
          "pipeline",
          input=_make_src_ds(),
          pipeline=(
              "read ! filter --where \"str_field='one'\" !"
              f" partition --output={tmp}/out --field str_field"
              " --format Parquet"
          ),
      )

      with ogr.Open(
          f"{tmp}/out/test/str_field=one/part_0000000001.parquet"
      ) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 2)

      self.assertIsNone(
          gdal.VSIStatL(f"{tmp}/out/test/str_field=two/part_0000000001.parquet")
      )

  def test_flat_scheme(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with utilities_util.vsimem_dir("partition_flat") as tmp:
      out_dir = f"{tmp}/out"
      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
          scheme="flat",
      )

      with ogr.Open(f"{out_dir}/test_one_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFIDColumn(), "my_fid")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 1)
        self.assertEqual(feat["int64 field"], 1234567890123)
        self.assertEqual(feat["real_field"], 1.5)
        self.assertEqual(feat.GetFID(), 10)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), "POINT (1 2)")
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "one")
        self.assertEqual(feat["int_field"], 2)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(f"{out_dir}/test_two_0000000001.gpkg") as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["str_field"], "two")
        self.assertEqual(feat["int_field"], 1)
        self.assertIsNone(lyr.GetNextFeature())

      with ogr.Open(f"{out_dir}/non%20spatial___NULL___0000000001.gpkg") as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

      gdal.Mkdir(f"{out_dir}/subdir", 0o755)
      with self.assertRaisesRegex(
          RuntimeError,
          "it does not look like a directory generated by this utility",
      ):
        gdal.Run(
            "vector",
            "partition",
            input=_make_src_ds(),
            output=out_dir,
            format="GPKG",
            field="str_field",
            scheme="flat",
            overwrite=True,
        )

      gdal.Rmdir(f"{out_dir}/subdir")

      gdal.Run(
          "vector",
          "partition",
          input=_make_src_ds(),
          output=out_dir,
          format="GPKG",
          field="str_field",
          scheme="flat",
          pattern="my_{LAYER_NAME}_%d_{FIELD_VALUE}",
          overwrite=True,
      )
      with ogr.Open(f"{out_dir}/my_test_1_one.gpkg") as ds:
        self.assertIsNotNone(ds)

  def test_pattern_errors(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    alg = gdal.Algorithm("vector", "partition")
    for pattern, err_regex in (
        (
            "",
            (
                "Value of argument 'pattern' is '', but should have at least 1"
                " character"
            ),
        ),
        ("x", "Missing '%' character in pattern"),
        ("%", "pattern value must include a single"),
        ("%%", "A single '%' character is expected in pattern"),
        ("%xd", "pattern value must include a single"),
        ("%5", "pattern value must include a single"),
        (
            "%11d",
            r"Number of digits in part number specifiation should be in"
            r" \[1,10\] range",
        ),
    ):
      with self.assertRaisesRegex(RuntimeError, err_regex):
        alg["pattern"] = pattern


if __name__ == "__main__":
  absltest.main()
