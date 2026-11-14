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

"""Tests for `gdal raster rgb-to-palette`.

Ported from upstream autotest/utilities/test_gdalalg_raster_rgb_to_palette.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator, Mapping
import contextlib

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> list[str]:
  return ["raster", "rgb-to-palette"]


class GdalalgRasterRgbToPaletteTest(gdaltest.TestCase, parameterized.TestCase):

  @contextlib.contextmanager
  def _assert_warning_raised(self) -> Iterator[None]:
    levels: list[int] = []

    def handler(lvl: int, unused_no: int, unused_msg: str) -> None:
      levels.append(lvl)

    gdal.PushErrorHandler(handler)
    try:
      yield
    finally:
      gdal.PopErrorHandler()
    self.assertIn(gdal.CE_Warning, levels)

  def _assert_paletted_small_world(
      self, ds: gdal.Dataset, src_ds: gdal.Dataset
  ) -> None:
    self.assertEqual(ds.RasterCount, 1)
    ct = ds.GetRasterBand(1).GetColorTable()
    self.assertIsNotNone(ct)
    self.assertEqual(ct.GetCount(), 256)
    self.assertEqual(ct.GetColorEntry(0), (188, 168, 100, 255))
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 14890)
    self.assertEqual(ds.GetGeoTransform(), src_ds.GetGeoTransform())
    self.assertIsSame(ds.GetSpatialRef(), src_ds.GetSpatialRef())
    self.assertEqual(ds.GetMetadata(), src_ds.GetMetadata())

  def test_nominal_and_mem_output(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    src_ds = gdal.Open(small_world)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None

    with utilities_util.vsimem_dir("raster_rgb_to_palette_nom") as tmp:
      with gdal.Run(_get_alg(), input=src_ds, output=f"{tmp}/out.tif") as alg:
        self._assert_paletted_small_world(alg.Output(), src_ds)

    with gdal.Run(_get_alg(), input=src_ds, output_format="MEM") as alg:
      self._assert_paletted_small_world(alg.Output(), src_ds)

  @parameterized.parameters(
      ({},),
      ({"COMPRESS": "LZW"},),
      ({"TILED": "YES", "COMPRESS": "LZW"},),
  )
  def test_nominal_with_progress(
      self, creation_option: Mapping[str, str]
  ) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    tab_pct = [0.0]

    def my_progress(pct: float, unused_msg: str, unused_data: object) -> bool:
      tab_pct[0] = pct
      return True

    src_ds = gdal.Open(small_world)
    self.assertIsNotNone(src_ds)
    assert src_ds is not None

    with utilities_util.vsimem_dir("raster_rgb_to_palette_prog") as tmp:
      out_tif = f"{tmp}/out.tif"
      self.assertTrue(
          gdal.Run(
              _get_alg(),
              input=src_ds,
              output=out_tif,
              creation_option=creation_option,
              progress=my_progress,
          )
      )

      with gdal.Open(out_tif) as ds:
        self.assertIsNone(gdal.VSIStatL(ds.GetDescription() + ".tmp.tif"))
        self.assertEqual(tab_pct[0], 1.0)
        self._assert_paletted_small_world(ds, src_ds)
        if "TILED" in creation_option:
          self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [256, 256])
        if creation_option.get("COMPRESS"):
          self.assertEqual(
              ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
              creation_option["COMPRESS"],
          )

  def test_error_cases(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    rgbsmall_tif = utilities_util.get_testdata_path(
        "../gcore/data/rgbsmall.tif"
    )
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )

    with utilities_util.vsimem_dir("raster_rgb_to_palette_err") as tmp:
      with self.assertRaisesRegex(
          RuntimeError, "Input dataset must have at least 3 bands"
      ):
        gdal.Run(_get_alg(), input=byte_tif, output=f"{tmp}/out.tif")

      with self.assertRaisesRegex(
          RuntimeError, "Non-byte band found and not supported"
      ):
        gdal.Run(
            _get_alg(),
            input=gdal.Translate(
                "", rgbsmall_tif, format="MEM", outputType=gdal.GDT_UInt16
            ),
            output=f"{tmp}/out.tif",
        )

    src_rrr = gdal.Translate("", small_world, format="MEM", bandList=[1, 1, 1])
    with self.assertRaisesRegex(RuntimeError, "Several Red bands found"):
      gdal.Run(_get_alg(), input=src_rrr, output_format="MEM")

    with self.assertRaisesRegex(
        RuntimeError, "Attempt to create new tiff file"
    ):
      gdal.Run(
          _get_alg(),
          input=small_world,
          output="/i_do/not/exist.tif",
      )

  def test_4_bands(self) -> None:
    rgba_tif = utilities_util.get_testdata_path(
        "../gcore/data/stefan_full_rgba.tif"
    )
    with utilities_util.vsimem_dir("raster_rgb_to_palette_4band") as tmp:
      with self._assert_warning_raised():
        with gdal.Run(
            _get_alg(), input=rgba_tif, output=f"{tmp}/out.tif"
        ) as alg:
          ds = alg.Output()
          self.assertEqual(ds.RasterCount, 1)
          ct = ds.GetRasterBand(1).GetColorTable()
          self.assertEqual(ct.GetCount(), 256)
          self.assertEqual(ct.GetColorEntry(0), (32, 12, 24, 255))
          self.assertEqual(ds.GetRasterBand(1).Checksum(), 41712)

  def test_color_interp_variants(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with utilities_util.vsimem_dir("raster_rgb_to_palette_ci") as tmp:
      src_undef = gdal.Translate(
          "",
          small_world,
          format="MEM",
          colorInterpretation=["undefined", "undefined", "undefined"],
      )
      with gdal.Run(
          _get_alg(), input=src_undef, output=f"{tmp}/out.tif"
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.RasterCount, 1)
        ct = ds.GetRasterBand(1).GetColorTable()
        self.assertEqual(ct.GetCount(), 256)
        self.assertEqual(ct.GetColorEntry(0), (188, 168, 100, 255))
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 14890)

    src_bgr = gdal.Translate("", small_world, format="MEM", bandList=[3, 2, 1])
    with gdal.Run(_get_alg(), input=src_bgr, output_format="MEM") as alg:
      ds = alg.Output()
      self.assertEqual(ds.RasterCount, 1)
      ct = ds.GetRasterBand(1).GetColorTable()
      self.assertEqual(ct.GetCount(), 256)
      self.assertEqual(ct.GetColorEntry(0), (188, 168, 100, 255))
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 14890)

    src_rg_undef = gdal.Translate(
        "",
        small_world,
        format="MEM",
        colorInterpretation=["red", "green", "undefined"],
    )
    with self._assert_warning_raised():
      with gdal.Run(_get_alg(), input=src_rg_undef, output_format="MEM") as alg:
        ds = alg.Output()
        self.assertEqual(ds.RasterCount, 1)
        ct = ds.GetRasterBand(1).GetColorTable()
        self.assertEqual(ct.GetCount(), 256)
        self.assertEqual(ct.GetColorEntry(0), (188, 168, 100, 255))
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 14890)

  def test_color_map_sources(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    color_txt = utilities_util.get_testdata_path(
        "data/color_paletted_red_green_0-255.txt"
    )
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    src_ds = gdal.Open(small_world)

    with gdal.Run(
        _get_alg(), input=src_ds, output_format="MEM", color_map=color_txt
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.RasterCount, 1)
      ct = ds.GetRasterBand(1).GetColorTable()
      self.assertEqual(ct.GetColorEntry(0), (255, 255, 255, 0))
      self.assertEqual(ct.GetColorEntry(1), (128, 128, 128, 255))

    with self.assertRaisesRegex(RuntimeError, "does not contain a color table"):
      gdal.Run(
          _get_alg(), input=src_ds, output_format="MEM", color_map=byte_tif
      )

    with self.assertRaisesRegex(RuntimeError, "Cannot find"):
      gdal.Run(
          _get_alg(),
          input=src_ds,
          output_format="MEM",
          color_map="/i_do/not/exist",
      )

  def test_colortable_from_png(self) -> None:
    if not utilities_util.has_driver("PNG"):
      self.skipTest("PNG driver not available")

    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    pct32_png = utilities_util.get_testdata_path(
        "../gcore/data/stefan_full_rgba_pct32.png"
    )
    src_ds = gdal.Open(small_world)
    with gdal.Run(
        _get_alg(), input=src_ds, output_format="MEM", color_map=pct32_png
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.RasterCount, 1)
      ct = ds.GetRasterBand(1).GetColorTable()
      self.assertEqual(ct.GetColorEntry(0), (0, 0, 0, 0))
      self.assertEqual(ct.GetColorEntry(1), (0, 0, 0, 1))

  def test_cannot_create_temp_file(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    src_ds = gdal.Open(small_world)
    with utilities_util.vsimem_dir("raster_rgb_to_palette_tmp") as tmp:
      with gdaltest.config_option(
          "GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET", "YES"
      ):
        with gdaltest.config_option("CPL_TMPDIR", "/i_do/not/exist"):
          with self.assertRaises(RuntimeError):
            gdal.Run(_get_alg(), input=src_ds, output=f"{tmp}/out.tif")


if __name__ == "__main__":
  absltest.main()
