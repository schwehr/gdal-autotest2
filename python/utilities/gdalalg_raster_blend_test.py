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

"""Tests for `gdal raster blend`.

Ported from upstream autotest/utilities/test_gdalalg_raster_blend.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import array
import os
import struct

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _is_sanitizer() -> bool:
  if any(
      key in os.environ
      for key in ("ASAN_OPTIONS", "MSAN_OPTIONS", "TSAN_OPTIONS")
  ):
    return True
  try:
    with open("/proc/self/maps", "r", encoding="utf-8") as f:
      maps = f.read()
    return any(
        token in maps
        for token in ("libasan", "libclang_rt.asan", "tsan", "msan")
    )
  except OSError:
    return False


class GdalalgRasterBlendTest(gdaltest.TestCase, parameterized.TestCase):

  def test_check_rgb_hsb_conversion(self) -> None:
    # n = 256 does full color space checking, but 64 keeps test runtime fast.
    n = 64
    self.assertEqual(n % 2, 0)

    nvalues = n**3
    rgba_ds = gdal.GetDriverByName("MEM").Create("", nvalues // 2, 2, 4)
    rgba_ds.SetProjection("WGS84")
    rgba_ds.SetGeoTransform([0, 1, 0, 0, 0, 1])

    r_vals = [
        255 if r == n - 1 else r * (256 // n)
        for r in range(n)
        for _ in range(n)
        for _ in range(n)
    ]
    g_vals = [
        255 if g == n - 1 else g * (256 // n)
        for _ in range(n)
        for g in range(n)
        for _ in range(n)
    ]
    b_vals = [
        255 if b == n - 1 else b * (256 // n)
        for _ in range(n)
        for _ in range(n)
        for b in range(n)
    ]
    rgba_ds.GetRasterBand(1).WriteRaster(
        0, 0, nvalues // 2, 2, array.array("B", r_vals).tobytes()
    )
    rgba_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand)
    rgba_ds.GetRasterBand(2).WriteRaster(
        0, 0, nvalues // 2, 2, array.array("B", g_vals).tobytes()
    )
    rgba_ds.GetRasterBand(3).WriteRaster(
        0, 0, nvalues // 2, 2, array.array("B", b_vals).tobytes()
    )
    rgba_ds.GetRasterBand(4).WriteRaster(
        0,
        0,
        nvalues // 2,
        2,
        array.array("B", [i & 255 for i in range(nvalues)]).tobytes(),
    )
    rgba_ds.BuildOverviews("NEAR", [2])

    grayscale_ds = gdal.GetDriverByName("MEM").Create("", nvalues // 2, 2)
    gray_vals = [
        max(
            255 if r == n - 1 else r * (256 // n),
            255 if g == n - 1 else g * (256 // n),
            255 if b == n - 1 else b * (256 // n),
        )
        for r in range(n)
        for g in range(n)
        for b in range(n)
    ]
    grayscale_ds.WriteRaster(
        0, 0, nvalues // 2, 2, array.array("B", gray_vals).tobytes()
    )
    grayscale_ds.BuildOverviews("NEAR", [2])

    with gdal.Run(
        "raster",
        "blend",
        input=rgba_ds,
        overlay=grayscale_ds,
        output_format="stream",
        operator="hsv-value",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, rgba_ds.RasterCount)
      self.assertEqual(out_ds.ReadRaster(), rgba_ds.ReadRaster())
      self.assertEqual(
          out_ds.GetRasterBand(1).GetColorInterpretation(),
          rgba_ds.GetRasterBand(1).GetColorInterpretation(),
      )
      self.assertIsSame(out_ds.GetSpatialRef(), rgba_ds.GetSpatialRef())
      self.assertEqual(out_ds.GetGeoTransform(), rgba_ds.GetGeoTransform())
      for band_idx in range(1, 5):
        self.assertEqual(
            out_ds.GetRasterBand(band_idx).ReadRaster(),
            rgba_ds.GetRasterBand(band_idx).ReadRaster(),
        )
      self.assertEqual(out_ds.GetRasterBand(1).GetOverviewCount(), 1)
      self.assertIsNotNone(out_ds.GetRasterBand(1).GetOverview(0))
      self.assertIsNone(out_ds.GetRasterBand(1).GetOverview(-1))
      self.assertIsNone(out_ds.GetRasterBand(1).GetOverview(1))

      subsampled_band = out_ds.GetRasterBand(1).ReadRaster(
          0, 0, nvalues // 2, 2, buf_xsize=nvalues // 4, buf_ysize=1
      )
      self.assertEqual(
          subsampled_band,
          rgba_ds.GetRasterBand(1).ReadRaster(
              0, 0, nvalues // 2, 2, buf_xsize=nvalues // 4, buf_ysize=1
          ),
      )
      self.assertEqual(
          subsampled_band, out_ds.GetRasterBand(1).GetOverview(0).ReadRaster()
      )

      subsampled = out_ds.ReadRaster(
          0, 0, nvalues // 2, 2, buf_xsize=nvalues // 4, buf_ysize=1
      )
      self.assertEqual(
          subsampled,
          rgba_ds.ReadRaster(
              0, 0, nvalues // 2, 2, buf_xsize=nvalues // 4, buf_ysize=1
          ),
      )
      self.assertEqual(
          subsampled,
          rgba_ds.GetRasterBand(1).GetOverview(0).GetDataset().ReadRaster(),
      )

      self.assertEqual(
          out_ds.GetRasterBand(1).ReadRaster(
              1.5, 0.25, 2.5, 1, buf_xsize=1, buf_ysize=1
          ),
          rgba_ds.GetRasterBand(1).ReadRaster(
              1.5, 0.25, 2.5, 1, buf_xsize=1, buf_ysize=1
          ),
      )
      self.assertEqual(
          out_ds.ReadRaster(
              1, 0, 3, 2, buf_xsize=5, buf_ysize=6, buf_type=gdal.GDT_UInt16
          ),
          rgba_ds.ReadRaster(
              1, 0, 3, 2, buf_xsize=5, buf_ysize=6, buf_type=gdal.GDT_UInt16
          ),
      )
      self.assertEqual(
          out_ds.GetRasterBand(2).ReadRaster(
              1, 0, 3, 2, buf_xsize=5, buf_ysize=6, buf_type=gdal.GDT_UInt16
          ),
          rgba_ds.GetRasterBand(2).ReadRaster(
              1, 0, 3, 2, buf_xsize=5, buf_ysize=6, buf_type=gdal.GDT_UInt16
          ),
      )
      self.assertEqual(
          out_ds.GetRasterBand(4).ReadBlock(0, 1),
          rgba_ds.GetRasterBand(4).ReadBlock(0, 1),
      )

      self.assertEqual(
          out_ds.ReadRaster(buf_pixel_space=2)[::2],
          rgba_ds.ReadRaster(buf_pixel_space=2)[::2],
      )
      for band_idx in range(1, 4):
        self.assertEqual(
            out_ds.GetRasterBand(band_idx).ReadRaster(buf_pixel_space=2)[::2],
            rgba_ds.GetRasterBand(band_idx).ReadRaster(buf_pixel_space=2)[::2],
        )

      for i in range(n):
        self.assertEqual(
            out_ds.ReadRaster(i * n, 0, 7, 1),
            rgba_ds.ReadRaster(i * n, 0, 7, 1),
        )
        self.assertEqual(
            out_ds.ReadRaster(i + n // 2 * n, 0, 7, 1),
            rgba_ds.ReadRaster(i + n // 2 * n, 0, 7, 1),
        )
        self.assertEqual(
            out_ds.ReadRaster(i + n // 2 * n * (n - 1) - 8, 0, 7, 1),
            rgba_ds.ReadRaster(i + n // 2 * n * (n - 1) - 8, 0, 7, 1),
        )

    grayscale_ds.GetRasterBand(1).GetOverview(0).Fill(0)
    with gdal.Run(
        "raster",
        "blend",
        input=rgba_ds,
        overlay=grayscale_ds,
        output_format="stream",
        operator="hsv-value",
    ) as alg:
      out_ds = alg.Output()
      subsampled = out_ds.ReadRaster(
          0, 0, nvalues // 2, 2, buf_xsize=nvalues // 4, buf_ysize=1
      )
      self.assertNotEqual(
          subsampled,
          rgba_ds.ReadRaster(
              0, 0, nvalues // 2, 2, buf_xsize=nvalues // 4, buf_ysize=1
          ),
      )

    for opacity in (0, 50):
      with gdal.Run(
          "raster",
          "blend",
          input=rgba_ds,
          overlay=grayscale_ds,
          output_format="stream",
          opacity=opacity,
          operator="hsv-value",
      ) as alg:
        self.assertEqual(alg.Output().ReadRaster(), rgba_ds.ReadRaster())

    with gdal.Run(
        "raster",
        "blend",
        input=rgba_ds,
        overlay=rgba_ds,
        output_format="stream",
        operator="hsv-value",
    ) as alg:
      self.assertEqual(alg.Output().ReadRaster(), rgba_ds.ReadRaster())

    rgb_ds = gdal.Translate("", rgba_ds, bandList=[1, 2, 3], format="MEM")
    with gdal.Run(
        "raster",
        "blend",
        input=rgb_ds,
        overlay=rgb_ds,
        output_format="stream",
        operator="hsv-value",
    ) as alg:
      self.assertEqual(alg.Output().ReadRaster(), rgb_ds.ReadRaster())

    grayscale_alpha_ds = gdal.Translate(
        "", grayscale_ds, bandList=[1, "mask"], format="MEM"
    )
    with gdal.Run(
        "raster",
        "blend",
        input=rgb_ds,
        overlay=grayscale_alpha_ds,
        output_format="stream",
        operator="hsv-value",
    ) as alg:
      self.assertEqual(alg.Output().ReadRaster(), rgb_ds.ReadRaster())

  def test_invalid_input_ds(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    input_ds = mem_drv.Create("", 1, 1, 2)
    grayscale_ds = mem_drv.Create("", 1, 1)
    with self.assertRaisesRegex(
        RuntimeError,
        "Operator hsv-value requires a 3-band or 4-band input dataset",
    ):
      gdal.Run(
          "raster",
          "blend",
          operator="hsv-value",
          input=input_ds,
          overlay=grayscale_ds,
          output_format="stream",
      )

    input_ds = mem_drv.Create("", 1, 1, 3, gdal.GDT_UInt16)
    with self.assertRaisesRegex(
        RuntimeError,
        "Only 1-band, 2-band, 3-band or 4-band Byte dataset supported as input",
    ):
      gdal.Run(
          "raster",
          "blend",
          input=input_ds,
          overlay=grayscale_ds,
          output_format="stream",
      )

    input_ds = mem_drv.Create("", 1, 1, 3)
    grayscale_ds = mem_drv.Create("", 1, 1, 1, gdal.GDT_UInt16)
    with self.assertRaisesRegex(
        RuntimeError,
        "Only 1-band, 2-band, 3-band or 4-band Byte dataset supported as"
        " overlay",
    ):
      gdal.Run(
          "raster",
          "blend",
          input=input_ds,
          overlay=grayscale_ds,
          output_format="stream",
      )

    for (in_w, in_h), (ov_w, ov_h) in (
        ((2, 10), (2, 9)),
        ((9, 2), (10, 2)),
    ):
      input_ds = mem_drv.Create("", in_w, in_h, 3)
      grayscale_ds = mem_drv.Create("", ov_w, ov_h, 1)
      with self.assertRaisesRegex(
          RuntimeError,
          "Input dataset and overlay dataset must have the same dimensions",
      ):
        gdal.Run(
            "raster",
            "blend",
            input=input_ds,
            overlay=grayscale_ds,
            output_format="stream",
        )

  @parameterized.parameters(
      (None, None, "dataset"),
      (None, None, "band"),
      ((1 << 31) / 2, 1, "dataset"),
      ((1 << 31) / 2, 1, "band"),
      ((1 << 31) - 1, (1 << 31) - 1, "dataset"),
      ((1 << 31) - 1, (1 << 31) - 1, "band"),
      (100 * 1000 * 1000, 100 * 1000 * 1000, "dataset"),
      (100 * 1000 * 1000, 100 * 1000 * 1000, "band"),
  )
  def test_out_of_memory(
      self,
      xsize: int | float | None,
      ysize: int | float | None,
      req_object: str,
  ) -> None:
    self.skipTest(
        "TCMalloc aborts on intentional huge OOM allocations in google3"
    )
    if xsize == 100 * 1000 * 1000 and _is_sanitizer():
      self.skipTest("skip huge allocation on sanitizer")

    with utilities_util.vsimem_dir("raster_blend_oom") as tmp:
      in_path = f"{tmp}/in.tif"
      gray_path = f"{tmp}/gray.tif"
      gtiff = gdal.GetDriverByName("GTiff")

      ds = gtiff.Create(
          in_path, 2, 2, 3, options=["COMPRESS=DEFLATE", "BLOCKYSIZE=2"]
      )
      ds.Close()
      with gdal.VSIFile(in_path, "rb") as f:
        data = f.read()[0:-3]
      with gdal.VSIFile(in_path, "wb") as f:
        f.write(data)

      ds = gtiff.Create(
          gray_path, 2, 2, options=["COMPRESS=DEFLATE", "BLOCKYSIZE=2"]
      )
      ds.Close()
      with gdal.VSIFile(gray_path, "rb") as f:
        data = f.read()[0:-3]
      with gdal.VSIFile(gray_path, "wb") as f:
        f.write(data)

      with gdal.Run(
          "raster",
          "blend",
          input=in_path,
          overlay=gray_path,
          output_format="stream",
          operator="hsv-value",
      ) as alg:
        out_ds = alg.Output()
        req_obj = out_ds if req_object == "dataset" else out_ds.GetRasterBand(1)
        for _ in range(2):
          with self.assertRaises(Exception):
            req_obj.ReadRaster(
                buf_xsize=xsize,
                buf_ysize=ysize,
                buf_pixel_space=1,
                buf_line_space=1,
                buf_band_space=1,
            )
        req_obj = None
        out_ds = None

  def test_src_over(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    input_rgba_ds = mem_drv.Create("", 100, 1, 4)
    input_rgba_ds.GetRasterBand(1).Fill(100)
    input_rgba_ds.GetRasterBand(2).Fill(150)
    input_rgba_ds.GetRasterBand(3).Fill(200)
    input_rgba_ds.GetRasterBand(4).Fill(250)
    input_rgb_ds = gdal.Translate(
        "", input_rgba_ds, bandList=[1, 2, 3], format="MEM"
    )

    overlay_rgba_ds = mem_drv.Create("", 100, 1, 4)
    overlay_rgba_ds.GetRasterBand(1).Fill(205)
    overlay_rgba_ds.GetRasterBand(2).Fill(220)
    overlay_rgba_ds.GetRasterBand(3).Fill(240)
    overlay_rgba_ds.GetRasterBand(4).Fill(250)
    overlay_rgb_ds = gdal.Translate(
        "", overlay_rgba_ds, bandList=[1, 2, 3], format="MEM"
    )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3 * 100, out_ds.ReadRaster()),
          struct.unpack("B" * 3 * 100, overlay_rgb_ds.ReadRaster()),
      )
      for band_idx in range(1, 4):
        self.assertEqual(
            struct.unpack(
                "B" * 100, out_ds.GetRasterBand(band_idx).ReadRaster()
            ),
            struct.unpack(
                "B" * 100, overlay_rgb_ds.GetRasterBand(band_idx).ReadRaster()
            ),
        )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
        opacity=0,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3 * 100, out_ds.ReadRaster()),
          struct.unpack("B" * 3 * 100, input_rgb_ds.ReadRaster()),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3, out_ds.ReadRaster(0, 0, 1, 1)),
          (
              (100 * 25 + 205 * 75) // 100,
              (150 * 25 + 220 * 75) // 100,
              (200 * 25 + 240 * 75) // 100,
          ),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_rgba_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3, out_ds.ReadRaster(0, 0, 1, 1)),
          (177, 201, 229),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgba_ds,
        overlay=overlay_rgba_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 4)
      self.assertEqual(
          struct.unpack("B" * 4, out_ds.ReadRaster(0, 0, 1, 1)),
          (177, 201, 229, 254),
      )
      for band_idx, expected_val in enumerate((177, 201, 229, 254), start=1):
        self.assertEqual(
            struct.unpack(
                "B" * 100, out_ds.GetRasterBand(band_idx).ReadRaster()
            ),
            (expected_val,) * 100,
        )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgba_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
        opacity=100,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 4)
      self.assertEqual(
          struct.unpack("B" * 4, out_ds.ReadRaster(0, 0, 1, 1)),
          (205, 220, 240, 255),
      )

    input_gray_alpha_ds = mem_drv.Create("", 100, 1, 2)
    input_gray_alpha_ds.GetRasterBand(1).Fill(125)
    input_gray_alpha_ds.GetRasterBand(2).Fill(250)

    input_gray_ds = mem_drv.Create("", 100, 1, 1)
    input_gray_ds.GetRasterBand(1).Fill(125)

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          struct.unpack("B", out_ds.ReadRaster(0, 0, 1, 1)),
          ((205 * 299 + 220 * 587 + 240 * 114) // 1000,),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
        opacity=0,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          struct.unpack("B", out_ds.ReadRaster(0, 0, 1, 1)), (125,)
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_rgb_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      gray = (205 * 299 + 220 * 587 + 240 * 114) // 1000
      self.assertEqual(
          struct.unpack("B", out_ds.ReadRaster(0, 0, 1, 1)),
          ((gray * 75 + 125 * 25) // 100,),
      )

    overlay_gray_alpha_ds = mem_drv.Create("", 100, 1, 2)
    overlay_gray_alpha_ds.GetRasterBand(1).Fill(213)
    overlay_gray_alpha_ds.GetRasterBand(2).Fill(250)

    overlay_gray_ds = mem_drv.Create("", 100, 1, 1)
    overlay_gray_ds.GetRasterBand(1).Fill(213)

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3, out_ds.ReadRaster(0, 0, 1, 1)),
          (213, 213, 213),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
        opacity=0,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3 * 100, out_ds.ReadRaster()),
          struct.unpack("B" * 3 * 100, input_rgb_ds.ReadRaster()),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_rgb_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 3)
      self.assertEqual(
          struct.unpack("B" * 3, out_ds.ReadRaster(0, 0, 1, 1)),
          (
              (100 * 25 + 213 * 75) // 100,
              (150 * 25 + 213 * 75) // 100,
              (200 * 25 + 213 * 75) // 100,
          ),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          struct.unpack("B" * 100, out_ds.ReadRaster()),
          struct.unpack("B" * 100, overlay_gray_ds.ReadRaster()),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
        opacity=0,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          struct.unpack("B" * 100, out_ds.ReadRaster()),
          struct.unpack("B" * 100, input_gray_ds.ReadRaster()),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          struct.unpack("B", out_ds.ReadRaster(0, 0, 1, 1)),
          ((213 * 75 + 125 * 25) // 100,),
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_alpha_ds,
        overlay=overlay_gray_alpha_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 2)
      self.assertEqual(
          struct.unpack("B" * 2, out_ds.ReadRaster(0, 0, 1, 1)), (190, 250)
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_alpha_ds,
        overlay=overlay_gray_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 2)
      self.assertEqual(
          struct.unpack("B" * 2, out_ds.ReadRaster(0, 0, 1, 1)), (191, 222)
      )

    with gdal.Run(
        "raster",
        "blend",
        input=input_gray_ds,
        overlay=overlay_gray_alpha_ds,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(
          struct.unpack("B", out_ds.ReadRaster(0, 0, 1, 1)), (189,)
      )

  def test_src_over_stefan_full_rgba(self) -> None:
    stefan = utilities_util.get_testdata_path(
        "../gcore/data/stefan_full_rgba.tif"
    )
    with gdal.Run(
        "raster",
        "blend",
        input=stefan,
        overlay=stefan,
        output_format="stream",
        opacity=75,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(
          [out_ds.GetRasterBand(i + 1).Checksum() for i in range(4)],
          [13392, 59283, 36167, 12963],
      )


if __name__ == "__main__":
  absltest.main()
