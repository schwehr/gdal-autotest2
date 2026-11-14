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

"""Tests for `gdal raster clip`.

Ported from upstream autotest/utilities/test_gdalalg_raster_clip.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator, Sequence
import contextlib

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["clip"]


@contextlib.contextmanager
def _capture_errors() -> Iterator[list[tuple[int, int, str]]]:
  errors: list[tuple[int, int, str]] = []

  def handler(err_level: int, err_no: int, err_msg: str) -> None:
    errors.append((err_level, err_no, err_msg))

  gdal.PushErrorHandler(handler)
  try:
    yield errors
  finally:
    gdal.PopErrorHandler()


class GdalalgRasterClipTest(gdaltest.TestCase, parameterized.TestCase):

  def _assert_gt_almost_equal(
      self, actual: Sequence[float], expected: Sequence[float]
  ) -> None:
    self.assertLen(actual, len(expected))
    for act_val, exp_val in zip(actual, expected):
      self.assertAlmostEqual(
          act_val, exp_val, delta=max(1e-8, abs(exp_val) * 1e-8)
      )

  def test_missing_bbox_or_like(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError, "clip: --bbox, --geometry or --like must be specified"
    ):
      alg.Run()

  def test_input_error(self) -> None:
    alg = _get_alg()
    alg["input"] = gdal.GetDriverByName("MEM").Create("", 1, 1)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["bbox"] = [440780, 3750200, 441860, 3751260]
    with self.assertRaisesRegex(
        RuntimeError,
        "Clipping is not supported on a raster without a geotransform",
    ):
      alg.Run()

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.SetGeoTransform([2, 1, -1, 49, -1, -1])

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["bbox"] = [440780, 3750200, 441860, 3751260]
    with self.assertRaisesRegex(
        RuntimeError,
        "Clipping is not supported on a raster whose geotransform has "
        "rotation terms",
    ):
      alg.Run()

  def test_bbox(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["bbox"] = [440780, 3750200, 441860, 3751260]
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 18)
    self.assertEqual(ds.RasterYSize, 18)
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "26711")
    self._assert_gt_almost_equal(
        ds.GetGeoTransform(), (440780, 60, 0, 3751260, 0, -60)
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 3695)

  def test_like(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["bbox"] = [440780, 3750200, 441860, 3751260]
    self.assertTrue(alg.Run())
    like_ds = utilities_util.get_arg_dataset(alg, "output")

    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["like"] = like_ds
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 18)
    self.assertEqual(ds.RasterYSize, 18)
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "26711")
    self._assert_gt_almost_equal(
        ds.GetGeoTransform(), (440780, 60, 0, 3751260, 0, -60)
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 3695)

  def test_like_postgis(self) -> None:
    if not utilities_util.has_driver("PostgreSQL"):
      self.skipTest("PostgreSQL driver not available")

    val = gdal.GetConfigOption("OGR_PG_CONNECTION_STRING", None)
    pg_connection_string = val if val is not None else "dbname=autotest"

    try:
      with gdal.OpenEx(
          f"PG:{pg_connection_string}", gdal.OF_VECTOR | gdal.OF_UPDATE
      ) as pg_ds:
        pg_ds.CreateLayer("test_gdalalg_raster_clip_like_postgis_one")
        pg_ds.CreateLayer("test_gdalalg_raster_clip_like_postgis_two")
    except RuntimeError:
      self.skipTest(f"Postgres not available using '{pg_connection_string}'")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    try:
      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["like"] = f"PG:{pg_connection_string}"
      with self.assertRaisesRegex(
          RuntimeError,
          "Only single layer dataset can be specified with --like when "
          "neither --like-layer or --like-sql have been specified",
      ):
        alg.Run()
    finally:
      with gdal.OpenEx(
          f"PG:{pg_connection_string}", gdal.OF_VECTOR | gdal.OF_UPDATE
      ) as pg_ds:
        pg_ds.ExecuteSQL(
            "DROP TABLE test_gdalalg_raster_clip_like_postgis_one CASCADE"
        )
        pg_ds.ExecuteSQL(
            "DROP TABLE test_gdalalg_raster_clip_like_postgis_two CASCADE"
        )

  def test_like_error(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["like"] = gdal.GetDriverByName("MEM").Create("", 1, 1)
    with self.assertRaisesRegex(RuntimeError, "has no geotransform matrix"):
      alg.Run()

    like_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    like_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])

    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["like"] = like_ds
    with self.assertRaisesRegex(RuntimeError, "has no CRS"):
      alg.Run()

  @parameterized.parameters(
      ("partially outside", True),
      ("partially outside", False),
      ("completely outside", True),
      ("completely outside", False),
  )
  def test_bbox_outside_source(
      self, bbox_pos: str, allow_bbox_outside_source: bool
  ) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    if allow_bbox_outside_source:
      alg["allow-bbox-outside-source"] = True
    if bbox_pos == "partially outside":
      alg["bbox"] = [440720 - 100, 3750120, 441920, 3751320]
    else:
      alg["bbox"] = [440720 - 100, 3750120 - 100, 440720 - 50, 3750120 - 50]

    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    if allow_bbox_outside_source:
      with _capture_errors() as errors:
        self.assertTrue(alg.Run())
      self.assertFalse(any(lvl != gdal.CE_Debug for lvl, _, _ in errors))
    else:
      with self.assertRaisesRegex(RuntimeError, bbox_pos):
        alg.Run()

  def test_bbox_crs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["bbox"] = [-117.631, 33.89, -117.628, 33.9005]
    alg["bbox-crs"] = "NAD27"
    alg["allow-bbox-outside-source"] = True
    with _capture_errors() as errors:
      self.assertTrue(alg.Run())
    self.assertFalse(any(lvl != gdal.CE_Debug for lvl, _, _ in errors))

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 6)
    self.assertEqual(ds.RasterYSize, 20)
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "26711")
    self._assert_gt_almost_equal(
        ds.GetGeoTransform(), (441620.0, 60.0, 0.0, 3751140.0, 0.0, -60.0)
    )

  def test_geometry(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["geometry"] = (
        "POLYGON ((440885 3750741, 441344 3750294, 441612 3750501, "
        "441773 3751203, 441545 3751254, 441576 3750847, 441576 3750847, "
        "440885 3750741))"
    )
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 16)
    self.assertEqual(ds.RasterYSize, 17)
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "26711")
    self._assert_gt_almost_equal(
        ds.GetGeoTransform(), (440840.0, 60.0, 0.0, 3751260.0, 0.0, -60.0)
    )

  def test_geometry_add_alpha(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Open(byte_tif) as src_ds:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["geometry"] = (
          "POLYGON ((440720 3750120,441920 3751320,441920 3750120,"
          "440720 3750120))"
      )
      alg["add-alpha"] = True
      with _capture_errors() as errors:
        self.assertTrue(alg.Run())
      self.assertTrue(any(lvl == gdal.CE_Warning for lvl, _, _ in errors))

      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.RasterXSize, 20)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "26711")
      self._assert_gt_almost_equal(
          ds.GetGeoTransform(), src_ds.GetGeoTransform()
      )
      self.assertEqual(
          ds.GetRasterBand(1).ReadRaster(0, 0, 10, 10), b"\x00" * 100
      )
      self.assertEqual(
          ds.GetRasterBand(1).ReadRaster(10, 10, 10, 10),
          src_ds.ReadRaster(10, 10, 10, 10),
      )
      self.assertEqual(
          ds.GetRasterBand(2).ReadRaster(0, 0, 10, 10), b"\x00" * 100
      )
      self.assertEqual(
          ds.GetRasterBand(2).ReadRaster(10, 10, 10, 10), b"\xff" * 100
      )

  def test_geometry_nodata(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    src_ds = gdal.Translate("", byte_tif, format="MEM")
    src_ds.GetRasterBand(1).SetNoDataValue(255)
    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["geometry"] = (
        "POLYGON ((440720 3750120,441920 3751320,441920 3750120,"
        "440720 3750120))"
    )
    with _capture_errors() as errors:
      self.assertTrue(alg.Run())
    self.assertTrue(any(lvl == gdal.CE_Warning for lvl, _, _ in errors))

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 20)
    self.assertEqual(ds.RasterYSize, 20)
    self.assertEqual(
        ds.GetRasterBand(1).ReadRaster(0, 0, 10, 10), b"\xff" * 100
    )
    self.assertEqual(
        ds.GetRasterBand(1).ReadRaster(10, 10, 10, 10),
        src_ds.ReadRaster(10, 10, 10, 10),
    )

  def test_wrong_geometry(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Open(byte_tif) as src_ds:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["geometry"] = "invalid"
      with self.assertRaisesRegex(
          RuntimeError,
          "Clipping geometry is neither a valid WKT or GeoJSON geometry",
      ):
        alg.Run()

  def test_geometry_upside_down(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 2)
    src_ds.SetGeoTransform([0, 1, 0, 0, 0, 1])
    src_ds.WriteRaster(0, 0, 1, 2, b"\x00\xff")

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["bbox"] = [0, 0, 1, 2]
    self.assertTrue(alg.Run())

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 1)
    self.assertEqual(ds.RasterYSize, 2)
    self.assertEqual(ds.ReadRaster(), b"\x00\xff")
    self._assert_gt_almost_equal(ds.GetGeoTransform(), src_ds.GetGeoTransform())

  def test_geometry_only_bbox(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Open(byte_tif) as src_ds:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["geometry"] = (
          "POLYGON ((440720 3750120,441920 3751320,441920 3750120,"
          "440720 3750120))"
      )
      alg["only-bbox"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.RasterXSize, 20)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_geometry_srs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    x1 = -117.641168620797
    y1 = 33.9023526904272
    x2 = -117.628110837847
    y2 = 33.8915970129623
    with gdal.Open(byte_tif) as src_ds:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["geometry"] = (
          f"POLYGON (({x1} {y1},{x1} {y2},{x2} {y2},{x2} {y1},{x1} {y1}))"
      )
      alg["geometry-crs"] = "EPSG:4267"
      alg["add-alpha"] = True
      alg["allow-bbox-outside-source"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.RasterXSize, 22)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4851)

  @parameterized.parameters(True, False)
  def test_geometry_outside_extent(
      self, allow_bbox_outside_source: bool
  ) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Open(byte_tif) as src_ds:
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["geometry"] = (
          "POLYGON ((440600 3750120,441920 3751320,441920 3750120,"
          "440600 3750120))"
      )
      if allow_bbox_outside_source:
        alg["allow-bbox-outside-source"] = True
        with _capture_errors() as errors:
          self.assertTrue(alg.Run())
        self.assertTrue(any(lvl == gdal.CE_Warning for lvl, _, _ in errors))
        ds = utilities_util.get_arg_dataset(alg, "output")
        self.assertEqual(ds.RasterXSize, 22)
        self.assertEqual(ds.RasterYSize, 20)
      else:
        with self.assertRaisesRegex(
            RuntimeError,
            "Clipping geometry is partially or totally outside the extent "
            "of the raster",
        ):
          alg.Run()

  def test_window(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output-format"] = "MEM"
    alg["window"] = [1, 2, 3, 4]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 3)
    self.assertEqual(ds.RasterYSize, 4)
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 105)

  def test_window_invalid(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()

    with self.assertRaisesRegex(
        RuntimeError,
        "Value of 'window' should be col,line,width,height with width > 0 "
        "and height > 0",
    ):
      alg["window"] = [1, 2, 0, 4]

    with self.assertRaisesRegex(
        RuntimeError,
        "Value of 'window' should be col,line,width,height with width > 0 "
        "and height > 0",
    ):
      alg["window"] = [1, 2, 3, 0]

    alg["input"] = byte_tif
    alg["output-format"] = "MEM"
    alg["window"] = [1, 2, 3, 4]
    alg["add-alpha"] = True
    with self.assertRaisesRegex(
        RuntimeError, "clip: 'alpha' argument is not supported with 'window'"
    ):
      alg.Run()


if __name__ == "__main__":
  absltest.main()
