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

"""Tests for `gdal raster tile`.

Ported from upstream autotest/utilities/test_gdalalg_raster_tile.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Callable, Iterator, Mapping, Sequence
import contextlib
import json
import os
import re
import resource
import struct
import sys
from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["tile"]


def _get_gdal_bin_dir() -> str:
  return os.path.dirname(utilities_util.get_gdal_path())


def _get_effective_cpus() -> int:
  val = gdal.GetConfigOption("GDAL_NUM_THREADS", None)
  if val and val != "ALL_CPUS":
    return int(val)
  return gdal.GetNumCPUs()


def _has_get_current_thread_count_and_fork() -> bool:
  return (
      sys.platform in ("linux", "darwin")
      or sys.platform.startswith("freebsd")
      or sys.platform.startswith("netbsd")
  )


@contextlib.contextmanager
def _config_options(options: Mapping[str, str | None]) -> Iterator[None]:
  saved = {k: gdal.GetConfigOption(k) for k in options}
  for k, v in options.items():
    gdal.SetConfigOption(k, v)
  try:
    yield
  finally:
    for k, v in saved.items():
      gdal.SetConfigOption(k, v)


@contextlib.contextmanager
def _error_handler(
    handler: Callable[[int, int, str], None],
) -> Iterator[None]:
  gdal.PushErrorHandler(handler)
  try:
    yield
  finally:
    gdal.PopErrorHandler()


class GdalalgRasterTileTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if not utilities_util.has_driver("PNG"):
      self.skipTest("PNG driver not available")

  def _assert_seq_approx(
      self,
      got: Sequence[float],
      expected: Sequence[float],
      rel_tol: float = 1e-6,
      abs_tol: float = 1e-12,
  ) -> None:
    self.assertEqual(len(got), len(expected))
    for g, e in zip(got, expected):
      tol = max(abs_tol, rel_tol * max(abs(g), abs(e)))
      self.assertAlmostEqual(g, e, delta=tol)

  @contextlib.contextmanager
  def _assert_warning_raised(self, pattern: str) -> Iterator[None]:
    warnings: list[str] = []

    def handler(err_class: int, err_no: int, msg: str) -> None:
      del err_no
      if err_class == gdal.CE_Warning:
        warnings.append(msg)

    with _error_handler(handler):
      yield
    self.assertTrue(
        any(re.search(pattern, msg) for msg in warnings),
        f"Expected warning matching {pattern!r}, got {warnings!r}",
    )

  def _check_12_bit_jpeg(self) -> None:
    drv = gdal.GetDriverByName("JPEG")
    if drv is None:
      self.skipTest("JPEG driver not available")
    dtypes = drv.GetMetadataItem(gdal.DMD_CREATIONDATATYPES) or ""
    if "UInt16" not in dtypes:
      self.skipTest("12bit JPEG not available")

  @parameterized.parameters((None, None), ("mercator", 256))
  def test_basic(self, tiling_scheme: str | None, tilesize: int | None) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir(
        f"test_gdalalg_raster_tile_basic_{tiling_scheme}_{tilesize}_"
    ) as vsimem:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      if tiling_scheme:
        alg["tiling-scheme"] = tiling_scheme
      if tilesize:
        alg["tile-size"] = tilesize
      alg["url"] = "http://example.com"
      alg["title"] = "my title"
      with gdaltest.config_option("GDAL_RASTER_TILE_HTML_PREC", "10"):
        self.assertTrue(alg.Run(my_progress))

      self.assertEqual(last_pct[0], 1.0)
      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "11/",
              "11/354/",
              "11/354/818.png",
              "leaflet.html",
              "mapml.mapml",
              "openlayers.html",
              "stacta.json",
          ],
      )

      with gdal.Open(f"{vsimem}/11/354/818.png") as ds:
        self.assertEqual(ds.RasterCount, 2)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(2)],
            [4160, 4415],
        )

      with gdal.VSIFile(f"{vsimem}/leaflet.html", "rb") as f:
        got = f.read()
      with open(
          utilities_util.get_testdata_path(
              "data/gdal_raster_tile_expected_leaflet.html"
          ),
          "rb",
      ) as f:
        self.assertEqual(got, f.read())

      if tiling_scheme is None:
        with gdal.VSIFile(f"{vsimem}/mapml.mapml", "rb") as f:
          got = f.read()
        with open(
            utilities_util.get_testdata_path(
                "data/gdal_raster_tile_expected_mapml.mapml"
            ),
            "rb",
        ) as f:
          self.assertEqual(got, f.read())

      with gdal.VSIFile(f"{vsimem}/openlayers.html", "rb") as f:
        got = f.read()
      with open(
          utilities_util.get_testdata_path(
              "data/gdal_raster_tile_expected_openlayers.html"
          ),
          "rb",
      ) as f:
        self.assertEqual(got, f.read())

      if tiling_scheme == "mercator":
        with gdal.VSIFile(f"{vsimem}/stacta.json", "rb") as f:
          got_json = json.loads(f.read())
        with open(
            utilities_util.get_testdata_path(
                "data/gdal_raster_tile_expected_stacta.json"
            ),
            "rb",
        ) as f:
          expected_json = json.loads(f.read())
        self._assert_seq_approx(got_json["bbox"], expected_json["bbox"])
        self.assertLen(got_json["geometry"]["coordinates"], 1)
        self.assertLen(got_json["geometry"]["coordinates"][0], 5)
        for i in range(5):
          self._assert_seq_approx(
              got_json["geometry"]["coordinates"][0][i],
              expected_json["geometry"]["coordinates"][0][i],
          )
        self._assert_seq_approx(
            got_json["properties"]["proj:transform"],
            expected_json["properties"]["proj:transform"],
        )
        got_json["bbox"] = expected_json["bbox"]
        got_json["geometry"]["coordinates"] = expected_json["geometry"][
            "coordinates"
        ]
        got_json["properties"]["proj:transform"] = expected_json["properties"][
            "proj:transform"
        ]
        self.assertEqual(got_json, expected_json)

      if utilities_util.has_driver("STACTA"):
        with gdal.Open(f"{vsimem}/stacta.json") as ds:
          self.assertEqual(ds.RasterXSize, 256)
          self.assertEqual(ds.RasterYSize, 256)
          self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "3857")
          self._assert_seq_approx(
              ds.GetGeoTransform(),
              (
                  -13110479.09147343,
                  76.43702828517625,
                  0.0,
                  4030983.1236470547,
                  0.0,
                  -76.43702828517625,
              ),
          )
          self.assertEqual(
              ds.GetMetadata_Dict(),
              {
                  "datetime": "1970-01-01T00:00:00.000Z",
                  "end_datetime": "9999-12-31T23:59:59.999Z",
                  "start_datetime": "0001-01-01T00:00:00.000Z",
              },
          )
          if utilities_util.has_curl():
            with self.assertRaisesRegex(
                RuntimeError,
                r"Cannot open http://example\.com/.*/11/354/818\.png",
            ):
              ds.GetRasterBand(1).Checksum()
          else:
            with self.assertRaises(RuntimeError):
              ds.GetRasterBand(1).Checksum()

  @parameterized.parameters(
      ("WorldCRS84Quad", True, True),
      ("geodetic", False, False),
  )
  def test_small_world_geodetic(
      self, tiling_scheme: str, xyz: bool, addalpha: bool
  ) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_geod") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["tiling-scheme"] = tiling_scheme
      if not xyz:
        alg["convention"] = "tms"
      if addalpha:
        alg["add-alpha"] = True
        nbands = 4
      else:
        nbands = 3
      with gdaltest.config_option("GDAL_RASTER_TILE_HTML_PREC", "10"):
        self.assertTrue(alg.Run())

      expected_files = (
          [
              "0/",
              "0/0/",
              "0/0/0.png",
              "0/1/",
              "0/1/0.png",
              "mapml.mapml",
              "openlayers.html",
              "stacta.json",
          ]
          if xyz
          else [
              "0/",
              "0/0/",
              "0/0/0.png",
              "0/1/",
              "0/1/0.png",
              "openlayers.html",
          ]
      )
      self.assertEqual(gdal.ReadDirRecursive(vsimem), expected_files)

      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, nbands)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self._assert_seq_approx(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(nbands)],
            [1315, 63955, 5106, 17849][0:nbands],
            abs_tol=1,
        )

      with gdal.Open(f"{vsimem}/0/1/0.png") as ds:
        self.assertEqual(ds.RasterCount, nbands)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self._assert_seq_approx(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(nbands)],
            [24456, 25846, 15674, 17849][0:nbands],
            abs_tol=1,
        )

      if xyz:
        with gdal.VSIFile(f"{vsimem}/mapml.mapml", "rb") as f:
          got = f.read()
        with open(
            utilities_util.get_testdata_path(
                "data/gdal_raster_tile_expected_geodetic_mapml.mapml"
            ),
            "rb",
        ) as f:
          self.assertEqual(got, f.read())

      with gdal.VSIFile(f"{vsimem}/openlayers.html", "rb") as f:
        got = f.read()
      with open(
          utilities_util.get_testdata_path(
              f"data/gdal_raster_tile_expected_openlayers_geodetic_{xyz}.html"
          ),
          "rb",
      ) as f:
        self.assertEqual(got, f.read())

  def test_invalid_output_directory(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    alg = _get_alg()
    alg["input"] = small_world
    alg["output"] = "/i_do/not/exist"
    with self.assertRaisesRegex(
        RuntimeError, "Cannot create output directory /i_do/not/exist"
    ):
      alg.Run()

  @parameterized.parameters(True, False)
  def test_small_world_google_crs84_quad(self, xyz: bool) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_gcrs84") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["tiling-scheme"] = "GoogleCRS84Quad"
      if not xyz:
        alg["convention"] = "tms"
      alg["webviewer"] = ["openlayers", "leaflet"]
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
              "openlayers.html",
          ],
      )

  def test_invalid_tile_min_max_xy_order(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_minmax_order") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["min-x"] = 1
      alg["max-x"] = 0
      with self.assertRaisesRegex(
          RuntimeError, "'min-x' must be lesser or equal to 'max-x'"
      ):
        alg.Run()

      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["min-y"] = 1
      alg["max-y"] = 0
      with self.assertRaisesRegex(
          RuntimeError, "'min-y' must be lesser or equal to 'max-y'"
      ):
        alg.Run()

  @parameterized.parameters("min-x", "min-y", "max-x", "max-y")
  def test_invalid_tile_coordinate_range(self, opt: str) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_coord_range") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["tiling-scheme"] = "GoogleCRS84Quad"
      alg[opt] = 2
      with self.assertRaisesRegex(
          RuntimeError, rf"'{opt}' value must be in \[0,1\] range"
      ):
        alg.Run()

  @parameterized.parameters(
      (
          0,
          0,
          1,
          1,
          [
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      ),
      (
          None,
          0,
          1,
          1,
          [
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      ),
      (
          0,
          None,
          1,
          1,
          [
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      ),
      (
          0,
          0,
          None,
          1,
          [
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      ),
      (
          0,
          0,
          1,
          None,
          [
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      ),
      (0, 0, 0, 0, ["1/", "1/0/", "1/0/0.png"]),
      (0, 0, 1, 0, ["1/", "1/0/", "1/0/0.png", "1/1/", "1/1/0.png"]),
      (0, 0, 0, 1, ["1/", "1/0/", "1/0/0.png", "1/0/1.png"]),
      (1, 0, 1, 1, ["1/", "1/1/", "1/1/0.png", "1/1/1.png"]),
      (1, 1, 1, 1, ["1/", "1/1/", "1/1/1.png"]),
  )
  def test_min_max_xy_coordinate(
      self,
      min_x: int | None,
      min_y: int | None,
      max_x: int | None,
      max_y: int | None,
      expected_files: list[str],
  ) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_minmax_xy") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["tiling-scheme"] = "GoogleCRS84Quad"
      alg["webviewer"] = "none"
      if min_x is not None:
        alg["min-x"] = min_x
      if min_y is not None:
        alg["min-y"] = min_y
      if max_x is not None:
        alg["max-x"] = max_x
      if max_y is not None:
        alg["max-y"] = max_y
      self.assertTrue(alg.Run())
      self.assertEqual(gdal.ReadDirRecursive(vsimem), expected_files)

  def test_min_max_xy_coordinate_not_intersecting(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_no_int") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", small_world, options="-of MEM -srcwin 0 0 199 200"
      )
      alg["output"] = vsimem
      alg["tiling-scheme"] = "GoogleCRS84Quad"
      alg["min-x"] = 1
      with self.assertRaisesRegex(
          RuntimeError,
          "Dataset extent not intersecting specified min/max X/Y tile"
          " coordinates",
      ):
        alg.Run()

  def test_min_max_xy_coordinate_not_intersecting_ok(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_no_int_ok") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", small_world, options="-of MEM -srcwin 0 0 199 200"
      )
      alg["output"] = vsimem
      alg["tiling-scheme"] = "GoogleCRS84Quad"
      alg["min-x"] = 1
      alg["no-intersection-ok"] = True
      with self._assert_warning_raised(
          "Dataset extent not intersecting specified min/max X/Y tile"
          " coordinates"
      ):
        self.assertTrue(alg.Run())
      self.assertIsNone(gdal.ReadDirRecursive(vsimem))

  @parameterized.parameters(
      (None, None, 4160, 1191),
      ("cubic", None, 4160, 1191),
      (None, "cubic", 4160, 1191),
      ("cubic", "cubic", 4160, 1191),
      ("cubic", "near", 4160, 1209),
      ("cubic", "q1", 4160, 1281),
      ("near", "cubic", 4217, 1223),
      ("q1", None, 4896, 1228),
      ("q1", "q1", 4896, 1228),
  )
  def test_nodata_and_resampling(
      self,
      resampling: str | None,
      overview_resampling: str | None,
      cs_11: int,
      cs_10: int,
  ) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611", noData=0
      )
      alg["output"] = tmpdir
      alg["output-format"] = "GTiff"
      alg["min-zoom"] = 10
      alg["skip-blank"] = True
      alg["webviewer"] = "none"
      if resampling:
        alg["resampling"] = resampling
      if overview_resampling:
        alg["overview-resampling"] = overview_resampling
      self.assertTrue(alg.Run())

      self.assertEqual(
          {x.replace("\\", "/") for x in gdal.ReadDirRecursive(tmpdir)},
          {
              "10/",
              "10/177/",
              "10/177/409.tif",
              "11/",
              "11/354/",
              "11/354/818.tif",
          },
      )

      with gdal.Open(os.path.join(tmpdir, "11", "354", "818.tif")) as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), cs_11)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 0)

      with gdal.Open(os.path.join(tmpdir, "10", "177", "409.tif")) as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), cs_10)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 0)

  def test_skip_blank(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "",
          byte_tif,
          format="MEM",
          outputSRS="EPSG:32611",
          scaleParams=[[0, 1, 0, 0]],
          noData=0,
      )
      alg["output"] = tmpdir
      alg["min-zoom"] = 10
      alg["skip-blank"] = True
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())

      self.assertEqual(
          {x.replace("\\", "/") for x in gdal.ReadDirRecursive(tmpdir)},
          {"10/", "10/177/"},
      )

  def test_invalid_output_format(self) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Invalid value for argument 'output-format'. Driver 'MEM' does not"
        " advertise any file format extension",
    ):
      alg["output-format"] = "MEM"

    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, "VRT output is not supported"):
      alg["output-format"] = "VRT"

  def test_png_not_available(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_nopng") as vsimem:
      drv = gdal.GetDriverByName("PNG")
      drv.Deregister()
      try:
        alg = _get_alg()
        alg["input"] = byte_tif
        alg["output"] = vsimem
        with self.assertRaisesRegex(
            RuntimeError,
            "Invalid value for argument 'output-format'. Driver 'PNG' does not"
            " exist",
        ):
          alg.Run()
      finally:
        drv.Register()

  def test_invalid_input(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_badin") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.GetDriverByName("MEM").Create("", 1, 1, 0)
      alg["output"] = vsimem
      with self.assertRaisesRegex(RuntimeError, "Invalid source dataset"):
        alg.Run()

  def test_palette_not_supported(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_pal_err") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
      src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_PaletteIndex)
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      with self.assertRaisesRegex(
          RuntimeError,
          "Datasets with color table not supported with non-nearest or"
          " non-mode resampling. Run 'gdal raster color-map' before or set the"
          " 'resampling' argument to 'nearest' or 'mode'",
      ):
        alg.Run()

  def test_palette_nearest(self) -> None:
    byte_pct = utilities_util.get_testdata_path("data/byte_pct.tif")
    with utilities_util.vsimem_dir("raster_tile_pal_ok") as vsimem:
      alg = _get_alg()
      alg["input"] = byte_pct
      alg["output"] = vsimem
      alg["resampling"] = "nearest"
      alg["min-zoom"] = 10
      alg["max-zoom"] = 11
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "10/",
              "10/177/",
              "10/177/409.png",
              "11/",
              "11/354/",
              "11/354/818.png",
              "leaflet.html",
              "mapml.mapml",
              "openlayers.html",
              "stacta.json",
          ],
      )

      with gdal.Open(f"{vsimem}/11/354/818.png") as ds:
        self.assertIsNotNone(ds.GetRasterBand(1).GetColorTable())
      with gdal.Open(f"{vsimem}/10/177/409.png") as ds:
        self.assertIsNotNone(ds.GetRasterBand(1).GetColorTable())

  def test_zoom_errors(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_zoom_err") as vsimem:
      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = vsimem
      alg["min-zoom"] = 1
      alg["max-zoom"] = 0
      with self.assertRaisesRegex(
          RuntimeError, "'min-zoom' must be lesser or equal to 'max-zoom'"
      ):
        alg.Run()

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = vsimem
      alg["max-zoom"] = 31
      with self.assertRaisesRegex(
          RuntimeError,
          r"max-zoom = 31 is invalid\. It must be in \[0,30\] range",
      ):
        alg.Run()

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = vsimem
      alg["min-zoom"] = 31
      with self.assertRaisesRegex(
          RuntimeError,
          r"Could not find an appropriate zoom level\. Perhaps min-zoom is too"
          r" large\?",
      ):
        alg.Run()

      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["max-zoom"] = 30
      with self.assertRaisesRegex(RuntimeError, "Too large zoom level"):
        alg.Run()

  @parameterized.parameters(
      ("PNG", gdal.GDT_Byte, 5, "Only up to 4 bands supported for PNG"),
      (
          "PNG",
          gdal.GDT_Int32,
          1,
          "Only Byte and UInt16 data types supported for PNG",
      ),
      ("JPEG", gdal.GDT_Byte, 5, "Only up to 4 bands supported for JPEG"),
      ("JPEG", gdal.GDT_Int32, 1, "Only Byte"),
      ("WEBP", gdal.GDT_Byte, 1, "Only 3 or 4 bands supported for WEBP"),
      ("WEBP", gdal.GDT_UInt16, 3, "Only Byte data type supported for WEBP"),
      (
          "GTX",
          gdal.GDT_Byte,
          1,
          "Attempt to create gtx file with unsupported data type 'Byte'",
      ),
  )
  def test_input_not_supported_by_output(
      self, output_format: str, datatype: int, nbands: int, message: str
  ) -> None:
    if not utilities_util.has_driver(output_format):
      self.skipTest(f"{output_format} driver not available")

    with utilities_util.vsimem_dir("raster_tile_unsup") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, nbands, datatype)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -180])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["output-format"] = output_format
      with self.assertRaisesRegex(RuntimeError, message):
        alg.Run()

  def test_input_not_supported_by_output_multithread(self) -> None:
    if not utilities_util.has_driver("GTX"):
      self.skipTest("GTX driver not available")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_gtx_mt") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["output-format"] = "GTX"
      alg["max-zoom"] = 2
      with self.assertRaisesRegex(
          RuntimeError,
          "Attempt to create gtx file with unsupported data type 'Byte'",
      ):
        alg.Run()

  def test_multithread(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_mt") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      alg.Run()
      self.assertLen(gdal.ReadDirRecursive(vsimem), 108)

  def test_multithread_progress(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_mt_prog") as vsimem:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      with gdaltest.config_option("GDAL_THRESHOLD_MIN_TILES_PER_JOB", "1"):
        alg.Run(my_progress)

      self.assertEqual(last_pct[0], 1.0)
      self.assertLen(gdal.ReadDirRecursive(vsimem), 108)

  def test_multithread_interrupt_in_base_tiles(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return False

      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = os.path.join(tmpdir, "subdir")
      alg["parallel-method"] = "thread"
      alg["max-zoom"] = 10
      with self.assertRaisesRegex(RuntimeError, "Process interrupted by user"):
        alg.Run(my_progress)
      self.assertNotEqual(last_pct[0], 1.0)

  def test_multithread_interrupt_in_ovr_tiles(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return pct > 0.75

      out_dir = os.path.join(tmpdir, "subdir")
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = out_dir
      alg["parallel-method"] = "thread"
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      with self.assertRaisesRegex(RuntimeError, "Process interrupted by user"):
        alg.Run(my_progress)
      self.assertNotEqual(last_pct[0], 1.0)
      self.assertNotEqual(len(gdal.ReadDirRecursive(out_dir)), 108)

  def test_spawn_auto(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_dir = os.path.join(tmpdir, "subdir")
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = out_dir
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      with _config_options({
          "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN": "1",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": _get_gdal_bin_dir(),
      }):
        alg.Run()

      self.assertLen(gdal.ReadDirRecursive(out_dir), 108)
      if _get_effective_cpus() >= 2:
        self.assertEqual(alg["parallel-method"], "spawn")
      else:
        self.assertEqual(alg["parallel-method"], "")

  def test_spawn_incompatible_source(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      alg = _get_alg()
      alg["input"] = gdal.Translate("", small_world, format="MEM")
      alg["output"] = os.path.join(tmpdir, "subdir")
      alg["parallel-method"] = "spawn"
      with self.assertRaisesRegex(
          RuntimeError,
          "Unnamed or memory dataset sources are not supported with spawn"
          " parallelization method",
      ):
        alg.Run()

  def test_spawn_incompatible_output(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_spawn_vsi") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["parallel-method"] = "spawn"
      with self.assertRaisesRegex(
          RuntimeError,
          "/vsimem/ output directory not supported with spawn parallelization"
          " method",
      ):
        alg.Run()

  def test_spawn_gdal_not_found(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = tmpdir
      alg["parallel-method"] = "spawn"
      with self.assertRaisesRegex(
          RuntimeError, "No 'gdal' binary can be found in '/i_do/not/exist'"
      ):
        with gdaltest.config_option("GDAL_PATH", "/i_do/not/exist"):
          alg.Run()

  def test_fork_auto(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    if not _has_get_current_thread_count_and_fork():
      self.skipTest("needs Linux, Mac, FreeBSD or netBSD")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    monothreaded = gdal.GetCurrentThreadCount() == 1
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_dir = os.path.join(tmpdir, "subdir")
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = out_dir
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      with _config_options({
          "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN": "1",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": "/i_do/not/exist",
      }):
        alg.Run()

      if monothreaded:
        self.assertEqual(alg["parallel-method"], "fork")
      else:
        self.assertEqual(alg["parallel-method"], "")
      self.assertLen(gdal.ReadDirRecursive(out_dir), 108)

  def test_fork_auto_incompatible_output(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    if not _has_get_current_thread_count_and_fork():
      self.skipTest("needs Linux, Mac, FreeBSD or netBSD")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_fork_vsi") as vsimem:
      out_dir = f"{vsimem}/subdir"
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = out_dir
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      with _config_options({
          "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN": "1",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": "/i_do/not/exist",
      }):
        alg.Run()

      self.assertNotEqual(alg["parallel-method"], "fork")
      self.assertLen(gdal.ReadDirRecursive(out_dir), 108)

  def test_fork_forced(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    if sys.platform == "win32":
      self.skipTest("Windows not supported for this test")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_dir = os.path.join(tmpdir, "subdir")
      alg = _get_alg()
      alg["input"] = gdal.Translate("", small_world, format="MEM")
      alg["output"] = out_dir
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      alg["parallel-method"] = "fork"
      with _config_options({
          "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN": "1",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
      }):
        alg.Run()

      self.assertEqual(alg["parallel-method"], "fork")
      self.assertLen(gdal.ReadDirRecursive(out_dir), 108)

  def test_fork_incompatible_source(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    if sys.platform == "win32":
      self.skipTest("Windows not supported for this test")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_fork_src") as vsimem:
      src_ds = gdal.Open(small_world)
      src_ds.SetDescription("")
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["parallel-method"] = "fork"
      with self.assertRaisesRegex(
          RuntimeError,
          "Unnamed non-MEM source are not supported with fork parallelization"
          " method",
      ):
        alg.Run()

  def test_fork_incompatible_output(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")
    if sys.platform == "win32":
      self.skipTest("Windows not supported for this test")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_fork_out") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["parallel-method"] = "fork"
      with self.assertRaisesRegex(
          RuntimeError,
          "/vsimem/ output directory not supported with fork parallelization"
          " method",
      ):
        alg.Run()

  def test_spawn_error_in_child(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      input_filename = os.path.join(tmpdir, "in.tif")
      gdal.Translate(input_filename, small_world)
      f = gdal.VSIFOpenL(input_filename, "rb+")
      self.assertIsNotNone(f)
      gdal.VSIFTruncateL(f, 4096)
      gdal.VSIFCloseL(f)

      alg = _get_alg()
      alg["input"] = input_filename
      alg["output"] = tmpdir
      alg["parallel-method"] = "spawn"
      alg["max-zoom"] = 3
      alg["num-threads"] = 2
      with self.assertRaisesRegex(RuntimeError, r"Child process.*failed"):
        with _config_options({
            "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
            "GDAL_PATH": _get_gdal_bin_dir(),
        }):
          alg.Run()

  def test_spawn_limit(self) -> None:
    if _get_effective_cpus() <= 2:
      self.skipTest("needs more than 2 CPUs")
    if sys.platform == "win32":
      self.skipTest("Windows not supported for this test")
    if resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 4096:
      self.skipTest("Limit for number of opened files is too high")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      fds = []
      num_threads = 3
      num_fds_per_thread = 3
      try:
        while True:
          try:
            fds.append(open("/dev/null", "rb"))
          except OSError:
            break
        for _ in range(1 + num_threads * num_fds_per_thread):
          fds.pop().close()

        alg = _get_alg()
        alg["input"] = small_world
        alg["output"] = tmpdir
        alg["parallel-method"] = "spawn"
        alg["min-zoom"] = 0
        alg["max-zoom"] = 3
        alg["num-threads"] = num_threads
        with _config_options({
            "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
            "GDAL_PATH": _get_gdal_bin_dir(),
        }):
          with self._assert_warning_raised(
              "Limiting the number of child workers to"
          ):
            alg.Run()
      finally:
        for f in fds:
          f.close()

      self.assertLen(gdal.ReadDirRecursive(tmpdir), 108)

  def test_spawn(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        self.assertGreaterEqual(pct, last_pct[0])
        last_pct[0] = pct
        return True

      got_spurious = [False]

      def my_handler(err_class: int, err_no: int, msg: str) -> None:
        del err_class, err_no
        if "Spurious" in msg:
          got_spurious[0] = True

      out_dir = os.path.join(tmpdir, "subdir")
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = out_dir
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      alg["parallel-method"] = "spawn"
      alg["metadata"] = {"foo": "bar"}
      with _config_options({
          "GDAL_RASTER_TILE_EMIT_SPURIOUS_CHARS": "YES",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": _get_gdal_bin_dir(),
      }):
        with _error_handler(my_handler):
          alg.Run(my_progress)

      self.assertEqual(last_pct[0], 1.0)
      self.assertLen(gdal.ReadDirRecursive(out_dir), 108)
      # WITHOUT_CPLDEBUG is defined in google3, so CPLDebug/CPL_LOG is a no-op.
      self.assertTrue(got_spurious[0])

  def test_spawn_interrupt_in_base_tiles(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return False

      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = os.path.join(tmpdir, "subdir")
      alg["max-zoom"] = 10
      with _config_options({
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": _get_gdal_bin_dir(),
      }):
        with self.assertRaisesRegex(
            RuntimeError, "Process interrupted by user"
        ):
          alg.Run(my_progress)
      self.assertNotEqual(last_pct[0], 1.0)

  def test_spawn_interrupt_in_ovr_tiles(self) -> None:
    if _get_effective_cpus() <= 1:
      self.skipTest("needs more than one CPU")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return pct > 0.75

      out_dir = os.path.join(tmpdir, "subdir")
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = out_dir
      alg["min-zoom"] = 0
      alg["max-zoom"] = 3
      with _config_options({
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": _get_gdal_bin_dir(),
      }):
        with self.assertRaisesRegex(
            RuntimeError, "Process interrupted by user"
        ):
          alg.Run(my_progress)
      self.assertNotEqual(last_pct[0], 1.0)
      self.assertNotEqual(len(gdal.ReadDirRecursive(out_dir)), 108)

  def test_jpeg_12_bit_ok(self) -> None:
    self._check_12_bit_jpeg()
    with utilities_util.vsimem_dir("raster_tile_j12_ok") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_UInt16)
      src_ds.GetRasterBand(1).Fill(4095)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -180])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["output-format"] = "JPEG"
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())

      with gdal.Open(f"{vsimem}/0/0/0.jpg") as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            ds.GetRasterBand(1).ComputeRasterMinMax(), (4095, 4095)
        )

  def test_jpeg_12_bit_too_large_values(self) -> None:
    self._check_12_bit_jpeg()
    with utilities_util.vsimem_dir("raster_tile_j12_val") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_UInt16)
      src_ds.GetRasterBand(1).Fill(4096)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -180])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["output-format"] = "JPEG"
      with self.assertRaisesRegex(
          RuntimeError, "JPEG output only supported up to 12 bits"
      ):
        alg.Run()

  def test_jpeg_12_bit_too_large_nbits(self) -> None:
    self._check_12_bit_jpeg()
    with utilities_util.vsimem_dir("raster_tile_j12_nbits") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_UInt16)
      src_ds.GetRasterBand(1).SetMetadataItem("NBITS", "13", "IMAGE_STRUCTURE")
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -180])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["output-format"] = "JPEG"
      with self.assertRaisesRegex(
          RuntimeError, "JPEG output only supported up to 12 bits"
      ):
        alg.Run()

  def test_missing_gt_and_srs(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_nogeoref") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
      src_ds.SetSpatialRef(osr.SpatialReference(epsg=32600))
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      with self.assertRaisesRegex(
          RuntimeError, "Ungeoreferenced datasets are not supported"
      ):
        alg.Run()

      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
      src_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      with self.assertRaisesRegex(
          RuntimeError, "Ungeoreferenced datasets are not supported"
      ):
        alg.Run()

  def test_not_earth_crs(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_nonearth") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +a=1000"))
      src_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      with self.assertRaisesRegex(
          RuntimeError,
          "Source and target ellipsoid do not belong to the same celestial"
          " body",
      ):
        alg.Run()

  def test_cannot_determine_target_extent(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_badext") as vsimem:
      src_ds = gdal.Warp("", small_world, options="-f MEM -t_srs +proj=ortho")
      src_ds = gdal.Translate("", src_ds, options="-f MEM -srcwin 0 0 1 1")
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      with self.assertRaisesRegex(
          RuntimeError, "Cannot determine extent of raster in target CRS"
      ):
        alg.Run()

  def test_extent_not_compatible_tile_matrix(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_nztm_err") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -0.001])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["tiling-scheme"] = "NZTM2000"
      with self.assertRaisesRegex(
          RuntimeError,
          "Extent of source dataset is not compatible with extent of tile"
          " matrix 7",
      ):
        alg.Run()

  def test_extent_not_compatible_tile_matrix_as_warning(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_nztm_warn") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -0.001])
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["tiling-scheme"] = "NZTM2000"
      alg["no-intersection-ok"] = True
      with self._assert_warning_raised(
          "Extent of source dataset is not compatible with extent of tile"
          " matrix 7"
      ):
        self.assertTrue(alg.Run())

  def test_addalpha_dstnodata_exclusive(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_excl") as vsimem:
      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = vsimem
      alg["add-alpha"] = True
      alg["dst-nodata"] = 0
      with self.assertRaisesRegex(
          RuntimeError, "'add-alpha' and 'dst-nodata' are mutually exclusive"
      ):
        alg.Run()

  def test_rgb(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_rgb") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem), ["0/", "0/0/", "0/0/0.png"]
      )
      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(3)],
            [24650, 23280, 16559],
        )

  def test_rgba_all_opaque(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_rgba_opaque") as vsimem:
      src_ds = gdal.Translate(
          "",
          small_world,
          options="-of MEM -b 1 -b 2 -b 3 -b mask -colorinterp_4 alpha",
      )
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["webviewer"] = "none"
      alg["min-zoom"] = 0
      alg["max-zoom"] = 1
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "0/",
              "0/0/",
              "0/0/0.png",
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      )

      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self._assert_seq_approx(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(3)],
            [25111, 24737, 16108],
            abs_tol=10,
        )

      with gdal.Open(f"{vsimem}/1/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self._assert_seq_approx(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(3)],
            [6241, 4330, 13703],
            abs_tol=10,
        )

  @parameterized.parameters(True, False)
  def test_rgba_partially_opaque(self, skip_blank: bool) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_rgba_part") as vsimem:
      src_ds = gdal.Translate(
          "",
          small_world,
          options="-of MEM -b 1 -b 2 -b 3 -b mask -colorinterp_4 alpha",
      )
      src_ds.GetRasterBand(4).WriteRaster(0, 0, 10, 10, b"\x00" * 100)
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["webviewer"] = "none"
      alg["min-zoom"] = 0
      alg["max-zoom"] = 1
      alg["skip-blank"] = skip_blank
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "0/",
              "0/0/",
              "0/0/0.png",
              "1/",
              "1/0/",
              "1/0/0.png",
              "1/0/1.png",
              "1/1/",
              "1/1/0.png",
              "1/1/1.png",
          ],
      )

      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 4)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self._assert_seq_approx(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(4)],
            [23761, 23390, 14544, 16124],
            abs_tol=10,
        )

      with gdal.Open(f"{vsimem}/1/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 4)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self._assert_seq_approx(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(4)],
            [289, 63903, 6812, 10052],
            abs_tol=10,
        )

  def test_rgba_all_transparent_skip_blank(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_rgba_transp") as vsimem:
      src_ds = gdal.Translate(
          "",
          small_world,
          options="-of MEM -b 1 -b 2 -b 3 -b mask -colorinterp_4 alpha",
      )
      src_ds.GetRasterBand(4).Fill(0)
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["webviewer"] = "none"
      alg["min-zoom"] = 0
      alg["max-zoom"] = 1
      alg["skip-blank"] = True
      self.assertTrue(alg.Run())
      self.assertEqual(gdal.ReadDirRecursive(vsimem), ["0/", "0/0/"])

  def test_no_alpha(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_noalpha") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["no-alpha"] = True
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem), ["11/", "11/354/", "11/354/818.png"]
      )
      with gdal.Open(f"{vsimem}/11/354/818.png") as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4160)

  def test_rgba_no_alpha(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_rgba_noalpha") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "",
          small_world,
          options="-of MEM -b 1 -b 2 -b 3 -b mask -colorinterp_4 alpha",
      )
      alg["output"] = vsimem
      alg["no-alpha"] = True
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem), ["0/", "0/0/", "0/0/0.png"]
      )
      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(3)],
            [24650, 23280, 16559],
        )

  def test_max_zoom_and_convention_tms(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_maxzoom") as vsimem:
      src_ds = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["max-zoom"] = 15
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())
      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "15/",
              "15/5676/",
              "15/5676/13100.png",
              "15/5676/13101.png",
              "15/5677/",
              "15/5677/13100.png",
              "15/5677/13101.png",
          ],
      )

    with utilities_util.vsimem_dir("raster_tile_tms") as vsimem:
      src_ds = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["max-zoom"] = 15
      alg["convention"] = "tms"
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())
      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "15/",
              "15/5676/",
              "15/5676/19666.png",
              "15/5676/19667.png",
              "15/5677/",
              "15/5677/19666.png",
              "15/5677/19667.png",
          ],
      )

  def test_min_zoom_metadata_aux_xml(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdaltest.config_option("GDAL_PAM_ENABLED", "YES"):
      with utilities_util.vsimem_dir("raster_tile_aux") as vsimem:
        alg = _get_alg()
        alg["input"] = gdal.Translate(
            "", byte_tif, format="MEM", outputSRS="EPSG:32611"
        )
        alg["output"] = vsimem
        alg["min-zoom"] = 10
        alg["aux-xml"] = True
        alg["copy-src-metadata"] = True
        alg["metadata"] = {"FOO": "BAR"}
        alg["webviewer"] = "none"
        self.assertTrue(alg.Run())

        self.assertEqual(
            gdal.ReadDirRecursive(vsimem),
            [
                "10/",
                "10/177/",
                "10/177/409.png",
                "10/177/409.png.aux.xml",
                "11/",
                "11/354/",
                "11/354/818.png",
                "11/354/818.png.aux.xml",
            ],
        )

        with gdal.Open(f"{vsimem}/10/177/409.png") as ds:
          self.assertEqual(
              ds.GetMetadata_Dict(), {"AREA_OR_POINT": "Area", "FOO": "BAR"}
          )
        with gdal.Open(f"{vsimem}/11/354/818.png") as ds:
          self.assertEqual(
              ds.GetMetadata_Dict(), {"AREA_OR_POINT": "Area", "FOO": "BAR"}
          )

  @parameterized.parameters(("GTiff", None), ("GTiff", 1024), ("COG", None))
  def test_output_format_gtiff(
      self, output_format: str, tile_size: int | None
  ) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    if not utilities_util.has_driver("COG"):
      self.skipTest("COG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_gtiff") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["output-format"] = output_format
      alg["min-zoom"] = 10
      alg["aux-xml"] = True
      alg["copy-src-metadata"] = True
      alg["metadata"] = {"FOO": "BAR"}
      alg["webviewer"] = "none"
      if tile_size:
        alg["tile-size"] = tile_size
      self.assertTrue(alg.Run())

      effective_tile_size = tile_size or 256
      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "10/",
              "10/177/",
              "10/177/409.tif",
              "11/",
              "11/354/",
              "11/354/818.tif",
          ],
      )

      expected_block = (
          [effective_tile_size, effective_tile_size]
          if effective_tile_size <= 512
          else [256, 256]
      )
      with gdal.Open(f"{vsimem}/10/177/409.tif") as ds:
        self.assertEqual(ds.RasterXSize, effective_tile_size)
        self.assertEqual(ds.RasterYSize, effective_tile_size)
        self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "3857")
        self._assert_seq_approx(
            list(ds.GetGeoTransform()),
            [
                -13110479.09147343,
                152.8740565703556 / effective_tile_size * 256,
                0.0,
                4030983.1236470547,
                0.0,
                -152.87405657035197 / effective_tile_size * 256,
            ],
        )
        self.assertEqual(
            ds.GetMetadata_Dict(), {"AREA_OR_POINT": "Area", "FOO": "BAR"}
        )
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )
        self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), expected_block)

      with gdal.Open(f"{vsimem}/11/354/818.tif") as ds:
        self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "3857")
        self._assert_seq_approx(
            list(ds.GetGeoTransform()),
            [
                -13110479.09147343,
                76.43702828517625 / effective_tile_size * 256,
                0.0,
                4030983.1236470547,
                0.0,
                -76.43702828517625 / effective_tile_size * 256,
            ],
        )
        self.assertEqual(
            ds.GetMetadata_Dict(), {"AREA_OR_POINT": "Area", "FOO": "BAR"}
        )
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )
        self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), expected_block)

  def test_resume(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_resume") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["min-zoom"] = 10
      alg["webviewer"] = "none"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "10/",
              "10/177/",
              "10/177/409.png",
              "11/",
              "11/354/",
              "11/354/818.png",
          ],
      )

      gdal.FileFromMemBuffer(f"{vsimem}/10/177/409.png", b"")
      gdal.FileFromMemBuffer(f"{vsimem}/11/354/818.png", b"")

      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["min-zoom"] = 10
      alg["resume"] = True
      self.assertTrue(alg.Run())

      self.assertEqual(gdal.VSIStatL(f"{vsimem}/10/177/409.png").size, 0)
      self.assertEqual(gdal.VSIStatL(f"{vsimem}/11/354/818.png").size, 0)

  def test_tilesize_and_too_large(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_tsize") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["tile-size"] = 512
      self.assertTrue(alg.Run())

      with gdal.Open(f"{vsimem}/11/354/818.png") as ds:
        self.assertEqual(ds.RasterCount, 2)
        self.assertEqual(ds.RasterXSize, 512)
        self.assertEqual(ds.RasterYSize, 512)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(2)],
            [17181, 17734],
        )

    with utilities_util.vsimem_dir("raster_tile_tsize_big") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 65536)
      src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +datum=WGS84"))
      src_ds.SetGeoTransform([-180, 360, 0, 90, 0, -180])

      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["tile-size"] = 32768
      alg["output-format"] = "GTiff"
      with self.assertRaisesRegex(
          RuntimeError,
          "Tile size and/or number of bands too large compared to available"
          " RAM",
      ):
        alg.Run()

  def test_cannot_reopen_tile(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_reopen") as vsimem:
      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["max-zoom"] = 1
      self.assertTrue(alg.Run())

      gdal.FileFromMemBuffer(f"{vsimem}/1/0/0.png", b"")

      alg = _get_alg()
      alg["input"] = small_world
      alg["output"] = vsimem
      alg["min-zoom"] = 0
      alg["max-zoom"] = 1
      alg["resume"] = True
      with self.assertRaisesRegex(
          RuntimeError, "exists but cannot be opened with PNG driver"
      ):
        alg.Run()

  def test_raster_tiling_scheme(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_raster") as vsimem:
      src_ds = gdal.Open(byte_tif)
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["tiling-scheme"] = "raster"
      alg["output-format"] = "GTiff"
      with gdaltest.config_option("GDAL_RASTER_TILE_HTML_PREC", "10"):
        self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          ["0/", "0/0/", "0/0/0.tif", "openlayers.html", "stacta.json"],
      )

      with gdal.Open(f"{vsimem}/0/0/0.tif") as ds:
        self.assertEqual(ds.RasterCount, 2)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(2)],
            [4333, 4898],
        )
        self.assertEqual(
            ds.GetRasterBand(1).ReadRaster(
                0, 0, src_ds.RasterXSize, src_ds.RasterYSize
            ),
            src_ds.ReadRaster(),
        )
        self._assert_seq_approx(
            list(ds.GetGeoTransform()), list(src_ds.GetGeoTransform())
        )
        self.assertIsSame(ds.GetSpatialRef(), src_ds.GetSpatialRef())

      with gdal.VSIFile(f"{vsimem}/openlayers.html", "rb") as f:
        got = f.read()
      with open(
          utilities_util.get_testdata_path(
              "data/gdal_raster_tile_raster_expected_openlayers.html"
          ),
          "rb",
      ) as f:
        self.assertEqual(got, f.read())

  def test_raster_ungeoreferenced(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_ungeo") as vsimem:
      src_ds = gdal.Open(byte_tif)
      wrk_ds = gdal.GetDriverByName("MEM").Create(
          "", src_ds.RasterXSize, src_ds.RasterYSize
      )
      wrk_ds.WriteRaster(
          0, 0, src_ds.RasterXSize, src_ds.RasterYSize, src_ds.ReadRaster()
      )

      alg = _get_alg()
      alg["input"] = wrk_ds
      alg["output"] = vsimem
      alg["tiling-scheme"] = "raster"
      alg["output-format"] = "GTiff"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          ["0/", "0/0/", "0/0/0.tif", "openlayers.html", "stacta.json"],
      )
      with gdal.Open(f"{vsimem}/0/0/0.tif") as ds:
        self.assertEqual(ds.RasterCount, 2)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(2)],
            [4333, 4898],
        )
        self.assertEqual(
            ds.GetRasterBand(1).ReadRaster(
                0, 0, src_ds.RasterXSize, src_ds.RasterYSize
            ),
            src_ds.ReadRaster(),
        )
        self._assert_seq_approx(
            list(ds.GetGeoTransform()), [0.0, 1.0, 0.0, 0.0, 0.0, -1.0]
        )
        self.assertIsNone(ds.GetSpatialRef())

  def test_raster_min_max_zoom(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_tile_r_mmz") as vsimem:
      src_ds = gdal.Open(small_world)
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["tiling-scheme"] = "raster"
      alg["min-zoom"] = 0
      alg["max-zoom"] = 2
      alg["resampling"] = "nearest"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "0/",
              "0/0/",
              "0/0/0.png",
              "1/",
              "1/0/",
              "1/0/0.png",
              "2/",
              "2/0/",
              "2/0/0.png",
              "2/1/",
              "2/1/0.png",
              "openlayers.html",
              "stacta.json",
          ],
      )

      with gdal.Open(f"{vsimem}/2/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 4)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(4)],
            [60550, 62572, 46338, 38489],
        )
        self.assertEqual(
            ds.ReadRaster(0, 0, 256, 200, band_list=[1, 2, 3]),
            src_ds.ReadRaster(0, 0, 256, 200),
        )

      with gdal.Open(f"{vsimem}/2/1/0.png") as ds:
        self.assertEqual(ds.RasterCount, 4)
        self.assertEqual(ds.RasterXSize, 256)
        self.assertEqual(ds.RasterYSize, 256)
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(4)],
            [54448, 61647, 44944, 38489],
        )
        self.assertEqual(
            ds.ReadRaster(0, 0, 400 - 256, 200, band_list=[1, 2, 3]),
            src_ds.ReadRaster(256, 0, 400 - 256, 200),
        )

      with gdal.Open(f"{vsimem}/1/0/0.png") as ds:
        self.assertEqual(
            ds.ReadRaster(0, 0, 200, 100, band_list=[1, 2, 3]),
            src_ds.ReadRaster(buf_xsize=200, buf_ysize=100),
        )

  def test_raster_kml(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_kml") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["min-zoom"] = 10
      alg["max-zoom"] = 11
      alg["resampling"] = "nearest"
      alg["kml"] = True
      with gdaltest.config_option("GDAL_RASTER_TILE_KML_PREC", "10"):
        self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "10/",
              "10/177/",
              "10/177/409.kml",
              "10/177/409.png",
              "11/",
              "11/354/",
              "11/354/818.kml",
              "11/354/818.png",
              "doc.kml",
              "leaflet.html",
              "mapml.mapml",
              "openlayers.html",
              "stacta.json",
          ],
      )

      for vsimem_rel, expected_rel in (
          ("doc.kml", "data/gdal_raster_tile_expected_byte_10_11_doc.kml"),
          (
              "10/177/409.kml",
              "data/gdal_raster_tile_expected_byte_10_11_10_177_409.kml",
          ),
          (
              "11/354/818.kml",
              "data/gdal_raster_tile_expected_byte_10_11_11_354_818.kml",
          ),
      ):
        with gdal.VSIFile(f"{vsimem}/{vsimem_rel}", "rb") as f:
          got = f.read()
        with open(utilities_util.get_testdata_path(expected_rel), "rb") as f:
          self.assertEqual(got, f.read())

      if utilities_util.has_driver("KMLSuperOverlay"):
        with gdal.Open(f"{vsimem}/doc.kml") as ds:
          self.assertEqual(ds.GetRasterBand(1).Checksum(), 4215)

  def test_raster_kml_with_gx_latlonquad(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_kml_quad") as vsimem:
      alg = _get_alg()
      alg["input"] = gdal.Translate(
          "", byte_tif, format="MEM", outputSRS="EPSG:32611"
      )
      alg["output"] = vsimem
      alg["tiling-scheme"] = "raster"
      alg["resampling"] = "nearest"
      alg["kml"] = True
      with gdaltest.config_option("GDAL_RASTER_TILE_KML_PREC", "10"):
        self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.ReadDirRecursive(vsimem),
          [
              "0/",
              "0/0/",
              "0/0/0.kml",
              "0/0/0.png",
              "doc.kml",
              "openlayers.html",
              "stacta.json",
          ],
      )

      for vsimem_rel, expected_rel in (
          ("doc.kml", "data/gdal_raster_tile_expected_byte_raster_doc.kml"),
          ("0/0/0.kml", "data/gdal_raster_tile_expected_byte_raster_0_0_0.kml"),
      ):
        with gdal.VSIFile(f"{vsimem}/{vsimem_rel}", "rb") as f:
          got = f.read()
        with open(utilities_util.get_testdata_path(expected_rel), "rb") as f:
          self.assertEqual(got, f.read())

  def test_excluded_values_error(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_tile_excl_err") as vsimem:
      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = vsimem
      alg["excluded-values"] = "0"
      with self.assertRaisesRegex(
          RuntimeError,
          "'excluded-values' can only be specified if 'resampling' is set to"
          " 'average'",
      ):
        alg.Run()

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = vsimem
      alg["excluded-values"] = "0"
      alg["resampling"] = "average"
      alg["overview-resampling"] = "near"
      with self.assertRaisesRegex(
          RuntimeError,
          "'excluded-values' can only be specified if 'overview-resampling' is"
          " set to 'average'",
      ):
        alg.Run()

  def test_excluded_values(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_excl_ok") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create(
          "", 256, 256, 3, gdal.GDT_Byte
      )
      src_ds.GetRasterBand(1).WriteRaster(
          0, 0, 2, 2, struct.pack("B" * 4, 10, 20, 30, 40)
      )
      src_ds.GetRasterBand(2).WriteRaster(
          0, 0, 2, 2, struct.pack("B" * 4, 11, 21, 31, 41)
      )
      src_ds.GetRasterBand(3).WriteRaster(
          0, 0, 2, 2, struct.pack("B" * 4, 12, 22, 32, 42)
      )
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      src_ds.SetSpatialRef(srs)
      max_gm = 20037508.342789244
      res_z1 = (2 * max_gm / 256) / 2
      src_ds.SetGeoTransform([-max_gm, res_z1, 0, max_gm, 0, -res_z1])

      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["resampling"] = "average"
      alg["excluded-values"] = "30,31,32"
      alg["excluded-values-pct-threshold"] = 50
      alg["min-zoom"] = 0
      alg["max-zoom"] = 1
      self.assertTrue(alg.Run())

      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(
            struct.unpack("B" * 4, ds.ReadRaster(0, 0, 1, 1)),
            (
                (10 + 20 + 40) // 3,
                (11 + 21 + 41) // 3,
                (12 + 22 + 42) // 3,
                255,
            ),
        )

  def test_nodata_values_pct_threshold(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_nodata_pct") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create(
          "", 256, 256, 3, gdal.GDT_Byte
      )
      for i in range(3):
        src_ds.GetRasterBand(i + 1).SetNoDataValue(20)
        src_ds.GetRasterBand(i + 1).WriteRaster(
            0, 0, 2, 2, struct.pack("B" * 4, 10, 20, 30, 40)
        )
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      src_ds.SetSpatialRef(srs)
      max_gm = 20037508.342789244
      res_z1 = (2 * max_gm / 256) / 2
      src_ds.SetGeoTransform([-max_gm, res_z1, 0, max_gm, 0, -res_z1])

      for threshold, expected in (
          (None, (round((10 + 30 + 40) / 3), 191)),
          (50, (round((10 + 30 + 40) / 3), 255)),
          (25, (0, 0)),
      ):
        alg = _get_alg()
        alg["input"] = src_ds
        alg["output"] = vsimem
        alg["resampling"] = "average"
        if threshold is not None:
          alg["nodata-values-pct-threshold"] = threshold
        alg["min-zoom"] = 0
        alg["max-zoom"] = 1
        self.assertTrue(alg.Run())
        with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
          raw = ds.ReadRaster(0, 0, 1, 1, band_list=[1, 4])
          self.assertEqual(struct.unpack("B" * 2, raw), expected)

  def test_red_tile_with_alpha(self) -> None:
    with utilities_util.vsimem_dir("raster_tile_red_alpha") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create(
          "", 256, 256, 4, gdal.GDT_Byte
      )
      src_ds.GetRasterBand(1).Fill(255)
      src_ds.GetRasterBand(2).Fill(0)
      src_ds.GetRasterBand(3).Fill(0)
      src_ds.GetRasterBand(4).Fill(255)
      src_ds.GetRasterBand(4).SetColorInterpretation(gdal.GCI_AlphaBand)
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      src_ds.SetSpatialRef(srs)
      max_gm = 20037508.342789244
      res_z0 = 2 * max_gm / 256
      src_ds.SetGeoTransform([-max_gm, res_z0, 0, max_gm, 0, -res_z0])

      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["max-zoom"] = 0
      self.assertTrue(alg.Run())

      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.GetRasterBand(1).ComputeRasterMinMax(), (255, 255))
        self.assertEqual(ds.GetRasterBand(2).ComputeRasterMinMax(), (0, 0))
        self.assertEqual(ds.GetRasterBand(3).ComputeRasterMinMax(), (0, 0))

  @parameterized.parameters(
      ("", 1),
      ("", 2),
      ("", 3),
      ("", 4),
      ("AVERAGE", 1),
      ("AVERAGE", 2),
      ("AVERAGE", 3),
      ("AVERAGE", 4),
      ("PAETH", 1),
      ("PAETH", 2),
      ("PAETH", 3),
      ("PAETH", 4),
  )
  def test_png_optim(self, png_filter: str, nbands: int) -> None:
    with utilities_util.vsimem_dir("raster_tile_png_opt") as vsimem:
      src_ds = gdal.GetDriverByName("MEM").Create(
          "", 256, 256, nbands, gdal.GDT_Byte
      )
      if nbands in (2, 4):
        src_ds.GetRasterBand(nbands).SetColorInterpretation(gdal.GCI_AlphaBand)
        src_ds.GetRasterBand(nbands).Fill(127)
        real_bands = nbands - 1
      else:
        real_bands = nbands

      for i in range(real_bands):
        buf = bytes(
            (i + j * 257 if (j // 256 % 4) == 3 else 0) % 256
            for j in range(256 * 256)
        )
        src_ds.GetRasterBand(i + 1).WriteRaster(0, 0, 256, 256, buf)

      srs = osr.SpatialReference()
      srs.ImportFromEPSG(3857)
      src_ds.SetSpatialRef(srs)
      max_gm = 20037508.342789244
      res_z0 = 2 * max_gm / 256
      src_ds.SetGeoTransform([-max_gm, res_z0, 0, max_gm, 0, -res_z0])

      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = vsimem
      alg["max-zoom"] = 0
      with gdaltest.config_option("GDAL_RASTER_TILE_PNG_FILTER", png_filter):
        self.assertTrue(alg.Run())

      with gdal.Open(f"{vsimem}/0/0/0.png") as ds:
        self.assertEqual(
            [ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)],
            [
                src_ds.GetRasterBand(i + 1).Checksum()
                for i in range(ds.RasterCount)
            ],
        )

  def test_png_optim_2(self) -> None:
    if not utilities_util.has_driver("JPEG"):
      self.skipTest("JPEG driver not available")
    paeth_jpg = utilities_util.get_testdata_path(
        "data/test_gdal_raster_tile_paeth.jpg"
    )
    with gdaltest.config_option("GDAL_PAM_ENABLED", "YES"):
      with utilities_util.vsimem_dir("raster_tile_png_opt2") as vsimem:
        alg = _get_alg()
        alg["input"] = paeth_jpg
        alg["output"] = vsimem
        alg.Run()
        size_auto = gdal.VSIStatL(f"{vsimem}/19/509173/334573.png").size

        alg = _get_alg()
        alg["input"] = paeth_jpg
        alg["output"] = vsimem
        with gdaltest.config_option("GDAL_RASTER_TILE_PNG_FILTER", "AVERAGE"):
          alg.Run()
        size_avg = gdal.VSIStatL(f"{vsimem}/19/509173/334573.png").size

        self.assertNotEqual(size_auto, size_avg)

  def test_pipeline(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_dirname = os.path.join(tmpdir, "subdir")
      with _config_options({
          "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN": "1",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": _get_gdal_bin_dir(),
      }):
        gdal.Run(
            "raster pipeline",
            pipeline=(
                f"mosaic {small_world} ! tile {out_dirname}"
                " --min-zoom=0 --max-zoom=3"
            ),
        )
      self.assertLen(gdal.ReadDirRecursive(out_dirname), 108)

  def test_pipeline_error(self) -> None:
    if _get_effective_cpus() <= 2:
      self.skipTest("needs more than 2 CPUs")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    src_ds = gdal.Translate("", small_world, format="MEM")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_dirname = os.path.join(tmpdir, "subdir")
      with _config_options({
          "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN": "1",
          "GDAL_THRESHOLD_MIN_TILES_PER_JOB": "1",
          "GDAL_PATH": _get_gdal_bin_dir(),
      }):
        with self.assertRaisesRegex(
            RuntimeError, "Cannot execute this pipeline in parallel mode"
        ):
          gdal.Run(
              "raster pipeline",
              input=src_ds,
              pipeline=f"mosaic ! tile {out_dirname} --min-zoom=0 --max-zoom=3",
          )


if __name__ == "__main__":
  absltest.main()
