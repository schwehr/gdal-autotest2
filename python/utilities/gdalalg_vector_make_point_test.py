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

"""Tests for `gdal vector make-point`.

Ported from upstream autotest/utilities/test_gdalalg_vector_make_point.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_make_point_alg() -> gdal.Algorithm:
  return gdal.Algorithm("vector", "make-point")


class GdalalgVectorMakePointTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      (False, False),
      (False, True),
      (True, False),
      (True, True),
  )
  def test_basic(self, use_z: bool, use_m: bool) -> None:
    nfeat = 3
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbNone)

    src_lyr.CreateField(ogr.FieldDefn("name", ogr.OFTString))
    src_lyr.CreateField(ogr.FieldDefn("my_x", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_y", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_m", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_z", ogr.OFTReal))

    feature = ogr.Feature(src_lyr.GetLayerDefn())
    for i in range(nfeat):
      feature["name"] = f"feat_{i}"
      feature["my_x"] = i
      feature["my_y"] = 2 * i
      feature["my_z"] = 3 * i
      feature["my_m"] = 4 * i
      src_lyr.CreateFeature(feature)

    make_point = _get_make_point_alg()
    make_point["input"] = src_ds
    make_point["x"] = "my_x"
    make_point["y"] = "my_y"
    if use_z:
      make_point["z"] = "my_z"
    if use_m:
      make_point["m"] = "my_m"
    make_point["dst-crs"] = "EPSG:6589"
    make_point["output"] = ""
    make_point["output-format"] = "MEM"

    self.assertTrue(make_point.Run())

    out_ds = make_point.Output()
    self.assertEqual(out_ds.GetLayerCount(), 1)
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetFeatureCount(), nfeat)

    out_defn = out_lyr.GetLayerDefn()
    self.assertEqual(out_defn.GetGeomFieldCount(), 1)

    expected_srs = osr.SpatialReference(epsg=6589)
    out_geom_defn = out_defn.GetGeomFieldDefn(0)
    self.assertIsSame(out_geom_defn.GetSpatialRef(), expected_srs)

    type_tokens = []
    if use_z:
      type_tokens.append("3D")
    if use_m:
      type_tokens.append("Measured")
    type_tokens.append("Point")
    self.assertEqual(
        ogr.GeometryTypeToName(out_geom_defn.GetType()), " ".join(type_tokens)
    )

    features = list(out_lyr)
    self.assertLen(features, nfeat)

    for i, feat in enumerate(features):
      self.assertEqual(feat["name"], f"feat_{i}")
      self.assertEqual(feat["my_x"], i)
      self.assertEqual(feat["my_y"], 2 * i)
      self.assertEqual(feat["my_z"], 3 * i)
      self.assertEqual(feat["my_m"], 4 * i)

      geom = feat.GetGeometryRef()
      self.assertIsSame(geom.GetSpatialReference(), expected_srs)

      if use_z and not use_m:
        self.assertEqual(
            geom.ExportToIsoWkt(), f"POINT Z ({i} {2 * i} {3 * i})"
        )
      elif use_z and use_m:
        self.assertEqual(
            geom.ExportToIsoWkt(), f"POINT ZM ({i} {2 * i} {3 * i} {4 * i})"
        )
      elif use_m:
        self.assertEqual(
            geom.ExportToIsoWkt(), f"POINT M ({i} {2 * i} {4 * i})"
        )
      else:
        self.assertEqual(geom.ExportToIsoWkt(), f"POINT ({i} {2 * i})")

  def test_invalid_srs(self) -> None:
    make_point = _get_make_point_alg()
    with self.assertRaisesRegex(RuntimeError, "Invalid value for 'dst-crs'"):
      make_point["dst-crs"] = "invalid"

  @parameterized.parameters(" 40m", "", " ")
  def test_invalid_values(self, value: str) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbNone)

    src_lyr.CreateField(ogr.FieldDefn("name", ogr.OFTString))
    src_lyr.CreateField(ogr.FieldDefn("my_x", ogr.OFTString))
    src_lyr.CreateField(ogr.FieldDefn("my_y", ogr.OFTString))

    feature = ogr.Feature(src_lyr.GetLayerDefn())
    feature["name"] = "my_feat"
    feature["my_x"] = 3
    feature["my_y"] = value
    src_lyr.CreateFeature(feature)

    make_point = _get_make_point_alg()
    make_point["input"] = src_ds
    make_point["x"] = "my_x"
    make_point["y"] = "my_y"
    make_point["output"] = ""
    make_point["output-format"] = "MEM"

    with self.assertRaisesRegex(RuntimeError, "Invalid value in field my_y"):
      make_point.Run()

  @parameterized.parameters("x", "y", "m", "z")
  def test_invalid_field_name(self, invalid_field: str) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbNone)

    src_lyr.CreateField(ogr.FieldDefn("name", ogr.OFTString))
    src_lyr.CreateField(ogr.FieldDefn("my_x", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_y", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_z", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_m", ogr.OFTReal))

    feature = ogr.Feature(src_lyr.GetLayerDefn())
    feature["my_x"] = 1
    feature["my_y"] = 2
    feature["my_z"] = 3
    feature["my_m"] = 4
    src_lyr.CreateFeature(feature)

    make_point = _get_make_point_alg()
    make_point["input"] = src_ds
    make_point["x"] = "my_x"
    make_point["y"] = "my_y"
    make_point["z"] = "my_z"
    make_point["m"] = "my_m"
    make_point[invalid_field] = "does_not_exist"
    make_point["output"] = ""
    make_point["output-format"] = "MEM"

    with self.assertRaisesRegex(
        RuntimeError,
        f"Specified {invalid_field.upper()} field name .* does not exist",
    ):
      make_point.Run()

  def test_remove_existing_geom_fields_and_no_input_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    src_lyr = src_ds.CreateLayer("test", geom_type=ogr.wkbPolygon)
    src_lyr.CreateField(ogr.FieldDefn("my_x", ogr.OFTReal))
    src_lyr.CreateField(ogr.FieldDefn("my_y", ogr.OFTReal))

    make_point = _get_make_point_alg()
    make_point["input"] = src_ds
    make_point["x"] = "my_x"
    make_point["y"] = "my_y"
    make_point["output"] = ""
    make_point["output-format"] = "MEM"
    self.assertTrue(make_point.Run())

    out_ds = make_point.Output()
    out_lyr = out_ds.GetLayer(0)
    self.assertIsNone(out_lyr.GetSpatialRef())
    self.assertEqual(out_lyr.GetLayerDefn().GetGeomFieldCount(), 1)

    make_point_empty = _get_make_point_alg()
    make_point_empty["input"] = gdal.GetDriverByName("MEM").CreateVector("")
    make_point_empty["x"] = "my_x"
    make_point_empty["y"] = "my_y"
    make_point_empty["output"] = ""
    make_point_empty["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "No input vector layer"):
      make_point_empty.Run()


if __name__ == "__main__":
  absltest.main()
