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

"""Tests for `gdal raster index`.

Ported from upstream autotest/utilities/test_gdalalg_raster_index.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_index_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["index"]


@contextlib.contextmanager
def _capture_errors() -> Iterator[list[int]]:
  err_types: list[int] = []

  def _handler(err_class: int, unused_err_no: int, unused_msg: str) -> None:
    err_types.append(err_class)

  gdal.ErrorReset()
  gdal.PushErrorHandler(_handler)
  try:
    yield err_types
  finally:
    gdal.PopErrorHandler()


class GdalalgRasterIndexTest(gdaltest.TestCase, parameterized.TestCase):

  def test_layer_must_be_specified(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_index_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError, "Argument 'layer' must be specified"
    ):
      alg.Run()

  def test_basic_index_and_progress(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    last_pct = [0.0]

    def my_progress(
        pct: float, unused_msg: str, unused_user_data: object
    ) -> bool:
      last_pct[0] = pct
      return True

    alg = _get_index_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "my_layer"
    self.assertTrue(alg.Run(my_progress))
    self.assertEqual(last_pct[0], 1.0)

    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayerByName("my_layer")
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "location")
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
    )
    feat = lyr.GetNextFeature()
    self.assertEqual(feat["location"], byte_tif)
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "POLYGON ((440720 3751320,441920 3751320,441920 3750120,"
        "440720 3750120,440720 3751320))",
    )

  def test_source_by_ref_rejected(self) -> None:
    alg = _get_index_alg()
    alg["input"] = gdal.GetDriverByName("MEM").Create("", 1, 1)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "my_layer"
    with self.assertRaisesRegex(
        RuntimeError, "Input datasets must be provided by name, not as object"
    ):
      alg.Run()

  def test_overwrite_append_and_update(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    uint16_tif = utilities_util.get_testdata_path("../gcore/data/uint16.tif")

    with utilities_util.vsimem_dir("raster_index_overwrite") as tmpdir:
      out_filename = f"{tmpdir}/out.shp"

      alg = _get_index_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
      self.assertEqual(lyr.GetFeatureCount(), 1)
      self.assertTrue(alg.Finalize())
      ds.Close()

      alg = _get_index_alg()
      alg["input"] = uint16_tif
      alg["output"] = out_filename
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg = _get_index_alg()
      alg["input"] = uint16_tif
      alg["output"] = out_filename
      alg["update"] = True
      with self.assertRaisesRegex(
          RuntimeError,
          r"Layer 'out' already exists\. Specify the --overwrite-layer option"
          r" to overwrite it, or --append to append to it\.",
      ):
        alg.Run()

      alg = _get_index_alg()
      alg["input"] = uint16_tif
      alg["output"] = out_filename
      alg["append"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
      self.assertEqual(lyr.GetFeatureCount(), 2)
      self.assertTrue(alg.Finalize())
      ds.Close()

      alg = _get_index_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["overwrite-layer"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
      self.assertEqual(lyr.GetFeatureCount(), 1)
      self.assertTrue(alg.Finalize())
      ds.Close()

      alg = _get_index_alg()
      alg["input"] = uint16_tif
      alg["output"] = out_filename
      alg["append"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
      self.assertEqual(lyr.GetFeatureCount(), 2)
      self.assertTrue(alg.Finalize())
      ds.Close()

      alg = _get_index_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
      self.assertEqual(lyr.GetFeatureCount(), 1)
      self.assertTrue(alg.Finalize())
      ds.Close()

  def test_recursive_filter_absolute_path_and_location_name(self) -> None:
    gcore_dir = os.path.dirname(
        utilities_util.get_testdata_path("../gcore/data/byte.tif")
    )
    alg = _get_index_alg()
    alg["input"] = gcore_dir
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "out"
    alg["recursive"] = True
    alg["filename-filter"] = "byt?.tif"
    alg["absolute-path"] = True
    alg["location-name"] = "path"
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), "path")
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
    )
    feat = lyr.GetNextFeature()
    self.assertIn("byte.tif", feat["path"])
    self.assertTrue(os.path.isabs(feat["path"]))
    self.assertTrue(os.path.exists(feat["path"]))

  def test_metadata(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_index_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "out"
    alg["metadata"] = {"foo": "bar"}
    alg["filename-filter"] = "byte.tif"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetMetadataItem("foo"), "bar")

  @parameterized.parameters((61, 0), (59, 1))
  def test_min_pixel_size(
      self, min_pixel_size: int, expected_count: int
  ) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_index_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "out"
    alg["min-pixel-size"] = min_pixel_size
    with _capture_errors() as errs:
      self.assertTrue(alg.Run())
    if min_pixel_size == 61:
      self.assertIn(gdal.CE_Warning, errs)
    else:
      self.assertEqual(errs, [])
    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetFeatureCount(), expected_count)

  def test_dst_crs_and_source_crs_field(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_index_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "out"
    alg["dst-crs"] = "EPSG:4267"
    alg["source-crs-field-name"] = "source_crs"
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "4267")
    feat = lyr.GetNextFeature()
    self.assertEqual(feat["source_crs"], "EPSG:26711")
    geom = feat.GetGeometryRef()
    expected = ogr.CreateGeometryFromWkt(
        "POLYGON ((-117.641168620797 33.9023526904272,"
        "-117.628190189534 33.9024195619211,"
        "-117.628110837847 33.8915970129623,"
        "-117.641087629972 33.8915301685907,"
        "-117.641168620797 33.9023526904272))"
    )
    ring_got = geom.GetGeometryRef(0)
    ring_exp = expected.GetGeometryRef(0)
    self.assertEqual(ring_got.GetPointCount(), ring_exp.GetPointCount())
    for i in range(ring_exp.GetPointCount()):
      self.assertAlmostEqual(ring_got.GetX(i), ring_exp.GetX(i), places=6)
      self.assertAlmostEqual(ring_got.GetY(i), ring_exp.GetY(i), places=6)

  def test_missing_input_and_skip_errors(self) -> None:
    alg = _get_index_alg()
    alg["input"] = "/i/do/not/exist"
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "out"
    alg["dst-crs"] = "EPSG:4267"
    with self.assertRaisesRegex(RuntimeError, "Unable to open /i/do/not/exist"):
      alg.Run()

    for dst_crs in ("EPSG:4267", None):
      alg = _get_index_alg()
      alg["input"] = "/i/do/not/exist"
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["output-layer"] = "out"
      if dst_crs is not None:
        alg["dst-crs"] = dst_crs
      alg["skip-errors"] = True
      with _capture_errors() as errs:
        self.assertTrue(alg.Run())
      self.assertIn(gdal.CE_Warning, errs)
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)


if __name__ == "__main__":
  absltest.main()
