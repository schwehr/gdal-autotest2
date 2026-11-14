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

"""Tests for `gdal vector grid`.

Ported from upstream autotest/utilities/test_gdalalg_vector_grid.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os
import sys
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


def _get_grid_alg(subalg: str) -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["grid"][subalg]


def _make_src_ds(geom_3d: bool) -> gdal.Dataset:
  src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
  src_lyr = src_ds.CreateLayer("test")
  src_lyr.CreateField(ogr.FieldDefn("z", ogr.OFTReal))
  for x, y, z in (
      (100, 1000, 500),
      (110, 1000, 400),
      (100, 1010, 300),
      (110, 1010, 600),
      (109, 1009, 610),
  ):
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    if geom_3d:
      feat.SetGeometry(ogr.CreateGeometryFromWkt(f"POINT Z({x} {y} {z})"))
    else:
      feat["z"] = z
      feat.SetGeometry(ogr.CreateGeometryFromWkt(f"POINT({x} {y})"))
    src_lyr.CreateFeature(feat)
  return src_ds


class GdalalgVectorGridTest(gdaltest.TestCase, parameterized.TestCase):

  def test_direct_grid_run_error(self) -> None:
    alg = gdal.GetGlobalAlgorithmRegistry()["vector"]["grid"]
    with self.assertRaisesRegex(
        RuntimeError, "method should not be called directly"
    ):
      alg.Run()

  @parameterized.parameters(
      ("invdist", False, {"zfield": "z"}, 51948, "success", None),
      (
          "invdist",
          False,
          {"input-layer": "test"},
          0,
          "warning",
          (
              "At least one geometry of layer 'test' lacks a Z component."
              " You may need to set the 'zfield' argument"
          ),
      ),
      ("invdist", True, {"input-layer": "test"}, 51948, "success", None),
      (
          "invdist",
          True,
          {"input-layer": "invalid"},
          None,
          "exception",
          'Unable to find layer "invalid"',
      ),
      ("invdist", True, {"sql": "SELECT * FROM test"}, 51948, "success", None),
      (
          "invdist",
          True,
          {"sql": "SELECT * FROM invalid"},
          None,
          "exception",
          "SELECT from table invalid failed",
      ),
      (
          "invdist",
          True,
          {"bbox": [99.9, 999.9, 110.1, 1010.1]},
          51948,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"bbox": [99.9, 999.9, 109.1, 1009.1]},
          65227,
          "success",
          None,
      ),
      ("invdist", True, {"zoffset": 10}, 54219, "success", None),
      ("invdist", True, {"zmultiply": 1.01}, 47965, "success", None),
      ("invdist", True, {}, 51948, "success", None),
      (
          "invdist",
          False,
          {},
          0,
          "warning",
          (
              "At least one geometry of layer 'test' lacks a Z component."
              " You may need to set the 'zfield' argument"
          ),
      ),
      ("invdist", True, {"power": 1.5}, 54471, "success", None),
      ("invdist", True, {"smoothing": 1.5}, 55409, "success", None),
      ("invdist", True, {"radius": 10}, 52638, "success", None),
      (
          "invdist",
          True,
          {"radius": 10, "radius1": 5, "radius2": 5},
          None,
          "exception",
          "Argument 'radius1' is mutually exclusive with 'radius'",
      ),
      ("invdist", True, {"radius": 5}, 32388, "success", None),
      (
          "invdist",
          True,
          {"radius1": 10},
          None,
          "exception",
          "'radius2' should be defined when 'radius1' is",
      ),
      (
          "invdist",
          True,
          {"radius2": 10},
          None,
          "exception",
          "'radius1' should be defined when 'radius2' is",
      ),
      (
          "invdist",
          True,
          {"radius1": 10, "radius2": 20},
          52085,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"radius1": 10, "radius2": 20, "angle": 90},
          51987,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"radius1": 10, "radius2": 10},
          52638,
          "success",
          None,
      ),
      ("invdist", True, {"radius": 5, "nodata": -1}, 23105, "success", None),
      (
          "invdist",
          True,
          {"min-points": 4, "radius": 10},
          4070,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"min-points": 1},
          None,
          "exception",
          (
              "'radius' or 'radius1' and 'radius2' should be defined when"
              " 'min-points' is"
          ),
      ),
      (
          "invdist",
          True,
          {"max-points": 2, "radius": 10},
          54334,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"max-points": 1},
          None,
          "exception",
          (
              "'radius' or 'radius1' and 'radius2' should be defined when"
              " 'max-points' is"
          ),
      ),
      (
          "invdist",
          True,
          {"min-points-per-quadrant": 1, "radius": 10},
          22047,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"max-points-per-quadrant": 1, "radius": 10},
          54602,
          "success",
          None,
      ),
      (
          "invdist",
          True,
          {"resolution": [1, 2]},
          None,
          "exception",
          "'resolution' should be defined when 'extent' is",
      ),
      ("invdistnn", True, {"radius": 20}, 51945, "success", None),
      ("invdistnn", True, {"radius": 10}, 52638, "success", None),
      (
          "invdistnn",
          True,
          {"radius": 20, "power": 1.5},
          54471,
          "success",
          None,
      ),
      (
          "invdistnn",
          True,
          {"radius": 20, "smoothing": 1.5},
          55409,
          "success",
          None,
      ),
      (
          "invdistnn",
          True,
          {"radius": 20, "max-points": 1},
          58195,
          "success",
          None,
      ),
      ("invdistnn", True, {"radius": 20, "min-points": 6}, 0, "success", None),
      (
          "invdistnn",
          True,
          {"radius": 20, "min-points-per-quadrant": 2},
          0,
          "success",
          None,
      ),
      (
          "invdistnn",
          True,
          {"radius": 20, "max-points-per-quadrant": 1},
          52361,
          "success",
          None,
      ),
      ("invdistnn", True, {"radius": 20, "nodata": -1}, 51945, "success", None),
      ("average", True, {}, 35730, "success", None),
      ("average", True, {"radius": 10}, 64540, "success", None),
      (
          "average",
          True,
          {"radius": 10, "radius1": 5, "radius2": 5},
          None,
          "exception",
          "Argument 'radius1' is mutually exclusive with 'radius'",
      ),
      ("average", True, {"radius": 5}, 17320, "success", None),
      (
          "average",
          True,
          {"radius1": 10},
          None,
          "exception",
          "'radius2' should be defined when 'radius1' is",
      ),
      (
          "average",
          True,
          {"radius2": 10},
          None,
          "exception",
          "'radius1' should be defined when 'radius2' is",
      ),
      (
          "average",
          True,
          {"radius1": 10, "radius2": 20},
          38493,
          "success",
          None,
      ),
      (
          "average",
          True,
          {"radius1": 10, "radius2": 20, "angle": 90},
          40297,
          "success",
          None,
      ),
      (
          "average",
          True,
          {"radius1": 10, "radius2": 10},
          64540,
          "success",
          None,
      ),
      ("average", True, {"radius": 5, "nodata": -1}, 8037, "success", None),
      (
          "average",
          True,
          {"radius": 20, "max-points": 3},
          None,
          "exception",
          (
              "'min-points-per-quadrant' and/or 'max-points-per-quadrant'"
              " should be defined when 'max-points' is"
          ),
      ),
      (
          "average",
          True,
          {"radius": 20, "max-points": 3, "min-points-per-quadrant": 1},
          0,
          "success",
          None,
      ),
      (
          "average",
          True,
          {"radius": 20, "max-points": 4, "min-points-per-quadrant": 1},
          61205,
          "success",
          None,
      ),
      (
          "average",
          True,
          {"radius": 20, "min-points": 1},
          35730,
          "success",
          None,
      ),
      ("average", True, {"radius": 20, "min-points": 6}, 0, "success", None),
      (
          "average",
          True,
          {"radius": 20, "max-points-per-quadrant": 1},
          61205,
          "success",
          None,
      ),
      ("nearest", True, {}, 58195, "success", None),
      ("nearest", True, {"radius": 5}, 15619, "success", None),
      ("nearest", True, {"radius": 5, "nodata": -1}, 6336, "success", None),
      ("nearest", True, {"radius1": 5, "radius2": 7}, 23049, "success", None),
      ("nearest", True, {"radius1": 7, "radius2": 5}, 25006, "success", None),
      ("linear", True, {}, 53743, "success", None),
      ("linear", True, {"extent": [90, 990, 120, 1120]}, 638, "success", None),
      (
          "linear",
          True,
          {"extent": [90, 990, 120, 1120], "radius": 1},
          21670,
          "success",
          None,
      ),
      (
          "linear",
          True,
          {"extent": [90, 990, 120, 1120], "radius": 0},
          19205,
          "success",
          None,
      ),
      (
          "linear",
          True,
          {"extent": [90, 990, 120, 1120], "radius": 0, "nodata": -1},
          20839,
          "success",
          None,
      ),
      ("minimum", True, {}, 35715, "success", None),
      ("minimum", True, {"radius": 5}, 23821, "success", None),
      ("minimum", True, {"radius": 5, "nodata": -1}, 14538, "success", None),
      (
          "minimum",
          True,
          {"radius1": 5, "radius2": 5},
          23821,
          "success",
          None,
      ),
      (
          "minimum",
          True,
          {"radius": 5, "min-points": 2},
          60234,
          "success",
          None,
      ),
      (
          "minimum",
          True,
          {"radius": 5, "min-points-per-quadrant": 2},
          0,
          "success",
          None,
      ),
      (
          "minimum",
          True,
          {"radius": 20, "max-points-per-quadrant": 1},
          53113,
          "success",
          None,
      ),
      ("maximum", True, {}, 59558, "success", None),
      ("range", True, {}, 17860, "success", None),
      ("count", True, {"radius": 5}, 5966, "success", None),
      ("average-distance", True, {}, 53719, "success", None),
      ("average-distance-points", True, {}, 23830, "success", None),
  )
  def test_regular(
      self,
      subalg: str,
      geom_3d: bool,
      options: dict[str, Any],
      checksum: int | None,
      ret_value: str,
      msg: str | None,
  ) -> None:
    if "bbox" in options and ogr.GetGEOSVersionMajor() == 0:
      self.skipTest("GEOS not available")
    if subalg == "linear" and not gdal.HasTriangulation():
      self.skipTest("qhull triangulation not available")

    alg = _get_grid_alg(subalg)
    alg["input"] = _make_src_ds(geom_3d)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    for k, v in options.items():
      alg[k] = v

    gdal.ErrorReset()
    if ret_value == "success":
      self.assertTrue(alg.Run())
      self.assertEqual(gdal.GetLastErrorMsg(), "")
    elif ret_value == "warning":
      with gdal.quiet_errors():
        self.assertTrue(alg.Run())
        self.assertIn(msg or "", gdal.GetLastErrorMsg())
    elif ret_value == "exception":
      with self.assertRaisesRegex(RuntimeError, msg or ""):
        alg.Run()
      return
    else:
      self.fail(f"Unexpected ret_value: {ret_value}")

    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertIsNotNone(checksum)
    self.assertAlmostEqual(ds.GetRasterBand(1).Checksum(), checksum, delta=3)
    if "nodata" in options:
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), options["nodata"])

  def test_progress(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    last_pct = [0.0]

    def on_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("vector_grid_progress") as tmp:
      alg = _get_grid_alg("invdist")
      alg["input"] = _make_src_ds(True)
      alg["output"] = f"{tmp}/out.tif"
      self.assertTrue(alg.ParseRunAndFinalize([], on_progress))
      self.assertEqual(last_pct[0], 1.0)

  def test_creation_option(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    with utilities_util.vsimem_dir("vector_grid_co") as tmp:
      alg = _get_grid_alg("invdist")
      alg["input"] = _make_src_ds(True)
      alg["output"] = f"{tmp}/out.tif"
      alg["creation-option"] = {"COMPRESS": "LZW"}
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(utilities_util.get_arg_name(alg, "output")) as ds:
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )

  def test_extent(self) -> None:
    alg = _get_grid_alg("invdist")
    alg["input"] = _make_src_ds(True)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["extent"] = [90, 990, 120, 1110]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        ds.GetGeoTransform(), (90.0, 0.1171875, 0.0, 1110.0, 0.0, -0.46875)
    )

  def test_size(self) -> None:
    alg = _get_grid_alg("invdist")
    alg["input"] = _make_src_ds(True)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["size"] = [10, 20]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 10)
    self.assertEqual(ds.RasterYSize, 20)

  def test_resolution(self) -> None:
    alg = _get_grid_alg("invdist")
    alg["input"] = _make_src_ds(True)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["resolution"] = [0.1, 0.2]
    alg["extent"] = [90, 990, 120, 1110]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetGeoTransform(), (90.0, 0.1, 0.0, 1110.0, 0.0, -0.2))

  def test_output_type(self) -> None:
    alg = _get_grid_alg("invdist")
    alg["input"] = _make_src_ds(True)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-data-type"] = "Float32"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float32)

  def test_crs(self) -> None:
    alg = _get_grid_alg("invdist")
    alg["input"] = _make_src_ds(True)
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["crs"] = "EPSG:4326"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "4326")

  def test_overwrite(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    with utilities_util.vsimem_dir("vector_grid_ow") as tmp:
      out_path = f"{tmp}/out.tif"
      with gdal.GetDriverByName("GTiff").Create(out_path, 1, 1):
        pass

      alg = _get_grid_alg("invdist")
      alg["input"] = _make_src_ds(True)
      alg["output"] = out_path
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg2 = _get_grid_alg("invdist")
      alg2["input"] = _make_src_ds(True)
      alg2["output"] = out_path
      alg2["overwrite"] = True
      self.assertTrue(alg2.Run())
      out_ds = utilities_util.get_arg_dataset(alg2, "output")
      self.assertEqual(out_ds.GetRasterBand(1).XSize, 256)

  def test_overwrite_failed_unlink(self) -> None:
    if sys.platform != "linux":
      self.skipTest("Linux-specific chmod test")
    if os.getuid() == 0:
      self.skipTest("Cannot test permission failure as root")
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_path = os.path.join(tmpdir, "out.tif")
      with gdal.GetDriverByName("GTiff").Create(out_path, 1, 1):
        pass

      os.chmod(tmpdir, 0o555)
      try:
        alg = _get_grid_alg("invdist")
        alg["input"] = _make_src_ds(True)
        alg["output"] = out_path
        alg["overwrite"] = True
        with self.assertRaises(RuntimeError):
          alg.Run()
      finally:
        os.chmod(tmpdir, 0o755)

  def test_autocomplete(self) -> None:
    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "vector",
        "grid",
        "invdist",
        "last_word_is_complete=false",
    ])
    self.assertEqual(code, 0)
    tokens = out.split(" ")
    self.assertIn("invdist", tokens)
    self.assertIn("invdistnn", tokens)

    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "vector",
        "grid",
        "invdist",
        "last_word_is_complete=true",
    ])
    self.assertEqual(code, 0)
    self.assertEqual(out.split(" "), [""])


if __name__ == "__main__":
  absltest.main()
