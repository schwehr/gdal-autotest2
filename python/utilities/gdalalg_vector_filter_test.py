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

"""Tests for `gdal vector filter`.

Ported from upstream autotest/utilities/test_gdalalg_vector_filter.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_filter_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["filter"]


class GdalalgVectorFilterTest(gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

  def test_no_filter(self) -> None:
    with utilities_util.vsimem_dir("vector_filter_none") as tmp:
      out_filename = f"{tmp}/out.shp"
      filter_alg = _get_filter_alg()
      self.assertTrue(
          filter_alg.ParseCommandLineArguments([self.poly_shp, out_filename])
      )
      self.assertTrue(filter_alg.Run())
      ds = utilities_util.get_arg_dataset(filter_alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
      self.assertTrue(filter_alg.Finalize())
      ds = None

      with gdal.OpenEx(out_filename) as out_ds:
        self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 10)

  def test_bbox(self) -> None:
    with utilities_util.vsimem_dir("vector_filter_bbox") as tmp:
      out_filename = f"{tmp}/out.shp"
      filter_alg = _get_filter_alg()
      self.assertTrue(
          filter_alg.ParseRunAndFinalize([
              "--bbox=479867,4762909,479868,4762910",
              self.poly_shp,
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_where_discard_and_accept_all(self) -> None:
    with utilities_util.vsimem_dir("vector_filter_where") as tmp:
      out_discard = f"{tmp}/discard.shp"
      self.assertTrue(
          _get_filter_alg().ParseRunAndFinalize(
              ["--where=0=1", self.poly_shp, out_discard]
          )
      )
      with gdal.OpenEx(out_discard) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

      out_accept = f"{tmp}/accept.shp"
      self.assertTrue(
          _get_filter_alg().ParseRunAndFinalize(
              ["--where=1=1", self.poly_shp, out_accept]
          )
      )
      with gdal.OpenEx(out_accept) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_where_error(self) -> None:
    with utilities_util.vsimem_dir("vector_filter_err") as tmp:
      out_filename = f"{tmp}/out.shp"
      filter_alg = _get_filter_alg()
      with self.assertRaisesRegex(
          RuntimeError, '"invalid" not recognised as an available field.'
      ):
        filter_alg.ParseRunAndFinalize(
            ["--where=invalid", self.poly_shp, out_filename]
        )

  def test_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    src_lyr.CreateField(ogr.FieldDefn("foo"))

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat["foo"] = "bar"
    src_lyr.CreateFeature(feat)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat["foo"] = "baz"
    src_lyr.CreateFeature(feat)

    other_lyr = src_ds.CreateLayer("other_layer")
    other_lyr.CreateField(ogr.FieldDefn("foo"))
    feat = ogr.Feature(other_lyr.GetLayerDefn())
    feat["foo"] = "baz"
    other_lyr.CreateFeature(feat)

    filter_alg = _get_filter_alg()
    filter_alg["input"] = src_ds
    filter_alg["active-layer"] = "the_layer"
    self.assertTrue(
        filter_alg.ParseCommandLineArguments(
            ["--where", "foo='bar'", "--of", "MEM", "--output", "memory_ds"]
        )
    )
    self.assertTrue(filter_alg.Run())

    out_ds = utilities_util.get_arg_dataset(filter_alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_feat = out_lyr.GetNextFeature()
    self.assertIsNotNone(out_feat)
    assert out_feat is not None
    self.assertEqual(out_feat["foo"], "bar")
    self.assertIsNone(out_lyr.GetNextFeature())

    out_lyr = out_ds.GetLayer(1)
    out_feat = out_lyr.GetNextFeature()
    self.assertIsNotNone(out_feat)
    assert out_feat is not None
    self.assertEqual(out_feat["foo"], "baz")


if __name__ == "__main__":
  absltest.main()
