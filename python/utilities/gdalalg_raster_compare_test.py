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

"""Tests for `gdal raster compare`.

Ported from upstream autotest/utilities/test_gdalalg_raster_compare.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import array
from collections.abc import Iterator, Sequence
import contextlib
import math
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


@contextlib.contextmanager
def _capture_warnings() -> Iterator[list[tuple[int, str]]]:
  captured: list[tuple[int, str]] = []

  def handler(err_class: int, _err_no: int, msg: str) -> None:
    captured.append((err_class, msg))

  gdal.PushErrorHandler(handler)
  try:
    yield captured
  finally:
    gdal.PopErrorHandler()


class GdalalgRasterCompareTest(gdaltest.TestCase, parameterized.TestCase):

  def _assert_warning_raised(
      self, warnings: Sequence[tuple[int, str]], substring: str
  ) -> None:
    self.assertTrue(
        any(
            err_cls == gdal.CE_Warning and substring in msg
            for err_cls, msg in warnings
        ),
        f"Expected warning containing {substring!r}, got {warnings!r}",
    )

  def test_same_file_and_pipeline(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run(
        "raster", "compare", input=byte_tif, reference=byte_tif
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    with gdal.Run(
        "raster",
        "pipeline",
        pipeline=f"read {byte_tif} ! compare --reference={byte_tif}",
    ) as alg:
      self.assertEqual(alg["output-string"], "")

  def test_progress_and_interrupted(self) -> None:
    rgbsmall = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")

    tab_pct = [0.0]

    def ok_progress(pct: float, _msg: str, _user_data: object) -> bool:
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    with gdal.Run(
        "raster",
        "compare",
        input=rgbsmall,
        reference=rgbsmall,
        skip_binary=True,
        progress=ok_progress,
    ) as alg:
      self.assertEqual(alg["output-string"], "")
    self.assertEqual(tab_pct[0], 1.0)

    tab_pct[0] = 0.0
    with gdal.Run(
        "raster",
        "pipeline",
        pipeline=(
            f"read {byte_tif} ! compare --reference={byte_tif} --skip-binary"
        ),
        progress=ok_progress,
    ) as alg:
      self.assertEqual(alg["output-string"], "")
    self.assertEqual(tab_pct[0], 1.0)

    tab_pct[0] = 0.0

    def cancel_progress(pct: float, _msg: str, _user_data: object) -> bool:
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return False

    with self.assertRaisesRegex(RuntimeError, "Interrupted by user"):
      gdal.Run(
          "raster",
          "compare",
          input=byte_tif,
          reference=byte_tif,
          skip_binary=True,
          progress=cancel_progress,
      )

  def test_same_content_not_same_binary(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_compare_diff_bin") as tmp:
      tmp_tif = f"{tmp}/tmp.tif"
      input_ds = gdal.Translate(tmp_tif, byte_tif)

      with gdal.Run(
          "raster", "compare", input=input_ds, reference=byte_tif
      ) as alg:
        lines = alg["output-string"].split("\n")[:-1]
        self.assertLen(lines, 1)
        self.assertTrue(
            lines[0].startswith(
                "Reference file has size 736 bytes, whereas input file has size"
            )
        )

      with gdal.Run(
          "raster",
          "pipeline",
          pipeline=f"read {tmp_tif} ! compare --reference={byte_tif}",
      ) as alg:
        self.assertTrue(
            alg["output-string"].startswith(
                "Reference file has size 736 bytes, whereas input file has size"
            )
        )

      for opt in ("skip_binary", "skip_all_optional"):
        with gdal.Run(
            "raster",
            "compare",
            input=input_ds,
            reference=byte_tif,
            **{opt: True},
        ) as alg:
          self.assertEqual(alg["output-string"], "")

      input_ds = None

  def test_binary_comparison(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    unnamed_ds = gdal.Translate("", byte_tif, format="MEM")

    with _capture_warnings() as warnings:
      with gdal.Run(
          "raster", "compare", input=byte_tif, reference=unnamed_ds
      ) as alg:
        self.assertEqual(alg["output-string"], "")
      self._assert_warning_raised(
          warnings,
          "Reference dataset has no name. Skipping binary file comparison",
      )

    with _capture_warnings() as warnings:
      with gdal.Run(
          "raster", "compare", input=unnamed_ds, reference=byte_tif
      ) as alg:
        self.assertEqual(alg["output-string"], "")
      self._assert_warning_raised(
          warnings,
          "Input dataset has no name. Skipping binary file comparison",
      )

    named_mem = gdal.Translate("named_but_mem", byte_tif, format="MEM")
    with _capture_warnings() as warnings:
      with gdal.Run(
          "raster", "compare", input=byte_tif, reference=named_mem
      ) as alg:
        self.assertEqual(alg["output-string"], "")
      self._assert_warning_raised(
          warnings,
          "Reference dataset is a in-memory dataset."
          " Skipping binary file comparison",
      )

    with _capture_warnings() as warnings:
      with gdal.Run(
          "raster", "compare", input=named_mem, reference=byte_tif
      ) as alg:
        self.assertEqual(alg["output-string"], "")
      self._assert_warning_raised(
          warnings,
          "Input dataset is a in-memory dataset."
          " Skipping binary file comparison",
      )

    with utilities_util.vsimem_dir("raster_compare_binary") as tmp:
      in_path = f"{tmp}/input.tif"
      ref_path = f"{tmp}/ref.tif"
      input_ds = gdal.Translate(in_path, byte_tif)
      ref_ds = gdal.Translate(ref_path, byte_tif)
      gdal.Unlink(in_path)

      with _capture_warnings() as warnings:
        with gdal.Run(
            "raster", "compare", input=input_ds, reference=ref_ds
        ) as alg:
          self.assertEqual(alg["output-string"], "")
        self._assert_warning_raised(
            warnings,
            f"Input dataset '{in_path}' is not a file."
            " Skipping binary file comparison",
        )

      input_ds = gdal.Translate(in_path, byte_tif)
      ref_ds = gdal.Translate(ref_path, byte_tif)
      gdal.Unlink(ref_path)

      with _capture_warnings() as warnings:
        with gdal.Run(
            "raster", "compare", input=input_ds, reference=ref_ds
        ) as alg:
          self.assertEqual(alg["output-string"], "")
        self._assert_warning_raised(
            warnings,
            f"Reference dataset '{ref_path}' is not a file."
            " Skipping binary file comparison",
        )

      input_ds = gdal.Translate(in_path, byte_tif)
      ref_ds = gdal.Translate(ref_path, byte_tif)
      # Populate GDAL's block cache before mutating the underlying file so pixel
      # comparison sees identical pixels while binary comparison detects the diff.
      input_ds.GetRasterBand(1).Checksum()
      with gdal.VSIFile(in_path, "r+b") as f:
        f.seek(0, os.SEEK_END)
        pos = f.tell()
        f.seek(pos - 1, os.SEEK_SET)
        f.write(b"\xFF")

      with gdal.Run(
          "raster", "compare", input=input_ds, reference=ref_ds
      ) as alg:
        self.assertEqual(
            alg["output-string"],
            "Reference file and input file differ at the binary level.\n",
        )
      input_ds = None
      ref_ds = None

  def test_crs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    input_ds = gdal.Translate("", byte_tif, format="MEM")
    input_ds.SetSpatialRef(None)
    ref_ds = gdal.Translate("", byte_tif, format="MEM")
    ref_ds.SetSpatialRef(None)

    with gdal.Run(
        "raster", "compare", input=input_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    ref_ds = gdal.Translate("", byte_tif, format="MEM")
    for opt in ("skip_crs", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=input_ds,
          reference=ref_ds,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

    with gdal.Run(
        "raster", "compare", input=input_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset has a CRS, but input dataset has none.\n",
      )

    with gdal.Run(
        "raster", "compare", input=ref_ds, reference=input_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset has no CRS, but input dataset has one.\n",
      )

    srs_4326 = osr.SpatialReference()
    srs_4326.ImportFromEPSG(4326)
    input_ds.SetSpatialRef(srs_4326)
    with gdal.Run(
        "raster", "compare", input=input_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      lines = alg["output-string"].split("\n")[:-1]
      self.assertLen(lines, 1)
      self.assertTrue(
          lines[0].startswith("Reference and input CRS are not equivalent.")
      )

  def test_geotransform(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    input_ds = mem_drv.Create("", 1, 1)
    ref_ds = mem_drv.Create("", 1, 1)

    with gdal.Run(
        "raster", "compare", input=input_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    input_ds.SetGeoTransform([1, 2, 3, 4, 5, 6])
    for opt in ("skip_geotransform", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=input_ds,
          reference=ref_ds,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

    with gdal.Run(
        "raster", "compare", input=input_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset has no geotransform, but input one has one.\n",
      )

    with gdal.Run(
        "raster", "compare", input=ref_ds, reference=input_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset has a geotransform, but input one has none.\n",
      )

    ref_ds.SetGeoTransform([1, 2, 3, 4, 5, -6])
    with gdal.Run(
        "raster", "compare", input=input_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Geotransform of reference and input dataset are not equivalent."
          " Reference geotransform is"
          " (1.000000,2.000000,3.000000,4.000000,5.000000,-6.000000)."
          " Input geotransform is"
          " (1.000000,2.000000,3.000000,4.000000,5.000000,6.000000)\n",
      )

  @parameterized.parameters(
      (gdal.GDT_Byte, "B", 255, 255),
      (gdal.GDT_Byte, "B", 255, 0),
      (gdal.GDT_Byte, "B", 0, 255),
      (gdal.GDT_Int8, "b", 127, 127),
      (gdal.GDT_Int8, "b", -128, -128),
      (gdal.GDT_Int8, "b", 127, -128),
      (gdal.GDT_Int8, "b", -128, 127),
      (gdal.GDT_UInt16, "H", 65535, 65535),
      (gdal.GDT_UInt16, "H", 65535, 0),
      (gdal.GDT_UInt16, "H", 0, 65535),
      (gdal.GDT_Int16, "h", 32767, 32767),
      (gdal.GDT_Int16, "h", -32768, -32768),
      (gdal.GDT_Int16, "h", 32767, -32768),
      (gdal.GDT_Int16, "h", -32768, 32767),
      (gdal.GDT_UInt32, "I", (1 << 32) - 1, (1 << 32) - 1),
      (gdal.GDT_UInt32, "I", (1 << 32) - 1, 0),
      (gdal.GDT_UInt32, "I", 0, (1 << 32) - 1),
      (gdal.GDT_Int32, "i", (1 << 31) - 1, (1 << 31) - 1),
      (gdal.GDT_Int32, "i", -(1 << 31), -(1 << 31)),
      (gdal.GDT_Int32, "i", (1 << 31) - 1, -(1 << 31)),
      (gdal.GDT_Int32, "i", -(1 << 31), (1 << 31) - 1),
      (gdal.GDT_UInt64, "Q", (1 << 64) - 1, (1 << 64) - 1),
      (gdal.GDT_UInt64, "Q", (1 << 64) - 1, 0),
      (gdal.GDT_UInt64, "Q", 0, (1 << 64) - 1),
      (gdal.GDT_Int64, "q", (1 << 63) - 1, (1 << 63) - 1),
      (gdal.GDT_Int64, "q", -(1 << 63), -(1 << 63)),
      (gdal.GDT_Int64, "q", (1 << 63) - 1, -(1 << 63)),
      (gdal.GDT_Int64, "q", -(1 << 63), (1 << 63) - 1),
      (gdal.GDT_Float16, "f", 1.5, 1.5),
      (gdal.GDT_Float16, "f", float("nan"), float("nan")),
      (gdal.GDT_Float16, "f", float("nan"), 0),
      (gdal.GDT_Float16, "f", 0, float("nan")),
      (gdal.GDT_Float16, "f", float("inf"), float("inf")),
      (gdal.GDT_Float16, "f", float("-inf"), float("-inf")),
      (gdal.GDT_Float16, "f", -1.5, 0),
      (gdal.GDT_Float16, "f", 0, -1.5),
      (gdal.GDT_Float32, "f", 1.5, 1.5),
      (gdal.GDT_Float32, "f", float("nan"), float("nan")),
      (gdal.GDT_Float32, "f", float("inf"), float("inf")),
      (gdal.GDT_Float32, "f", float("-inf"), float("-inf")),
      (gdal.GDT_Float32, "f", -1.5, 0),
      (gdal.GDT_Float32, "f", 0, -1.5),
      (gdal.GDT_Float64, "d", 1.5, 1.5),
      (gdal.GDT_Float64, "d", float("nan"), float("nan")),
      (gdal.GDT_Float64, "d", float("inf"), float("inf")),
      (gdal.GDT_Float64, "d", float("-inf"), float("-inf")),
      (gdal.GDT_Float64, "d", -1.5, 0),
      (gdal.GDT_Float64, "d", 0, -1.5),
      (
          gdal.GDT_CInt16,
          "h",
          [(1 << 15) - 1, -(1 << 15)],
          [(1 << 15) - 1, -(1 << 15)],
      ),
      (gdal.GDT_CInt16, "h", [(1 << 15) - 1, -(1 << 15)], [0, 0]),
      (
          gdal.GDT_CInt32,
          "i",
          [(1 << 31) - 1, -(1 << 31)],
          [(1 << 31) - 1, -(1 << 31)],
      ),
      (gdal.GDT_CInt32, "i", [(1 << 31) - 1, -(1 << 31)], [0, 0]),
      (
          gdal.GDT_CFloat16,
          "f",
          [float("nan"), float("nan")],
          [float("nan"), float("nan")],
      ),
      (gdal.GDT_CFloat16, "f", [1.5, -2.5], [1.5, -2.5]),
      (gdal.GDT_CFloat16, "f", [1.5, -2.5], [0, 0]),
      (gdal.GDT_CFloat32, "f", [1.5, -2.5], [1.5, -2.5]),
      (gdal.GDT_CFloat32, "f", [1.5, -2.5], [0, 0]),
      (gdal.GDT_CFloat64, "d", [1.5, -2.5], [1.5, -2.5]),
      (gdal.GDT_CFloat64, "d", [1.5, -2.5], [0, 0]),
  )
  def test_pixel(
      self,
      dt: int,
      stype: str,
      v1: float | Sequence[float],
      v2: float | Sequence[float],
  ) -> None:
    buf_type = dt
    if dt == gdal.GDT_Float16:
      buf_type = gdal.GDT_Float32
    elif dt == gdal.GDT_CFloat16:
      buf_type = gdal.GDT_CFloat32

    mem_drv = gdal.GetDriverByName("MEM")
    for band_interleaved in (False, True):
      nbands = 1 if band_interleaved else 16
      options = [] if band_interleaved else ["INTERLEAVE=PIXEL"]

      input_ds = mem_drv.Create("", 1, 1, nbands, dt, options=options)
      input_vals = v1 if isinstance(v1, Sequence) else [v1]
      input_ds.GetRasterBand(nbands).WriteRaster(
          0, 0, 1, 1, bytes(array.array(stype, input_vals)), buf_type=buf_type
      )

      ref_ds = mem_drv.Create("", 1, 1, nbands, dt, options=options)
      ref_vals = v2 if isinstance(v2, Sequence) else [v2]
      ref_ds.GetRasterBand(nbands).WriteRaster(
          0, 0, 1, 1, bytes(array.array(stype, ref_vals)), buf_type=buf_type
      )

      with gdal.Run(
          "raster",
          "compare",
          input=input_ds,
          reference=ref_ds,
          skip_binary=True,
      ) as alg:
        out_str = alg["output-string"]
        if isinstance(v1, Sequence) and isinstance(v2, Sequence):
          if all(math.isnan(x) for x in (*v1, *v2)) or list(v1) == list(v2):
            self.assertEqual(out_str, "")
          else:
            dist = math.hypot(v1[0] - v2[0], v1[1] - v2[1])
            self.assertIn(str(dist)[:7], out_str)
        elif not isinstance(v1, Sequence) and not isinstance(v2, Sequence):
          if v1 == v2 or (math.isnan(v1) and math.isnan(v2)):
            self.assertEqual(out_str, "")
          elif math.isnan(v1) or math.isnan(v2):
            self.assertNotEqual(out_str, "")
          else:
            self.assertIn(str(abs(v1 - v2)), out_str)

  def test_pixel_interleaved_progress(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create(
        "", 1, 1, 16, gdal.GDT_Byte, options=["INTERLEAVE=PIXEL"]
    )
    tab_pct = [0.0]

    def ok_progress(pct: float, _msg: str, _user_data: object) -> bool:
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    self.assertTrue(
        gdal.Run(
            "raster",
            "compare",
            input=ds,
            reference=ds,
            skip_binary=True,
            progress=ok_progress,
        )
    )
    self.assertEqual(tab_pct[0], 1.0)

    tab_pct[0] = 0.0

    def cancel_progress(pct: float, _msg: str, _user_data: object) -> bool:
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return False

    with self.assertRaisesRegex(RuntimeError, "Interrupted by user"):
      gdal.Run(
          "raster",
          "compare",
          input=ds,
          reference=ds,
          skip_binary=True,
          progress=cancel_progress,
      )

  def test_band_properties(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    # Band count
    with gdal.Run(
        "raster",
        "compare",
        input=mem_drv.Create("", 1, 1, 1),
        reference=mem_drv.Create("", 1, 1, 2),
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset has 2 band(s), but input dataset has 1\n",
      )

    # Width
    with gdal.Run(
        "raster",
        "compare",
        input=mem_drv.Create("", 1, 1, 1),
        reference=mem_drv.Create("", 2, 1, 1),
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset width is 2, but input dataset width is 1\n",
      )

    # Height
    with gdal.Run(
        "raster",
        "compare",
        input=mem_drv.Create("", 1, 1, 1),
        reference=mem_drv.Create("", 1, 2, 1),
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset height is 2, but input dataset height is 1\n",
      )

    # Data type
    with gdal.Run(
        "raster",
        "compare",
        input=mem_drv.Create("", 1, 1, 1, gdal.GDT_Byte),
        reference=mem_drv.Create("", 1, 1, 1, gdal.GDT_Int16),
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has data type Int16,"
          " but input band has data type Byte\n",
      )

    # Description
    in_desc = mem_drv.Create("", 1, 1, 1)
    in_desc.GetRasterBand(1).SetDescription("foo")
    ref_desc = mem_drv.Create("", 1, 1, 1)
    ref_desc.GetRasterBand(1).SetDescription("bar")
    with gdal.Run(
        "raster",
        "compare",
        input=in_desc,
        reference=ref_desc,
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has description bar,"
          " but input band has description foo\n",
      )

  def test_nodata(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")

    in_ds = mem_drv.Create("", 1, 1, 1)
    in_ds.GetRasterBand(1).SetNoDataValue(0)
    ref_ds = mem_drv.Create("", 1, 1, 1)
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has no nodata value,"
          " but input band has no data value 0.000000.\n"
          "Reference band 1 has mask flags = 1 ,"
          " but input band has mask flags = 8\n",
      )

    with gdal.Run(
        "raster", "compare", input=ref_ds, reference=in_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has nodata value 0.000000,"
          " but input band has none.\n"
          "Reference band 1 has mask flags = 8 ,"
          " but input band has mask flags = 1\n",
      )

    in_ds = mem_drv.Create("", 1, 1, 1)
    in_ds.GetRasterBand(1).SetNoDataValue(0)
    ref_ds = mem_drv.Create("", 1, 1, 1)
    ref_ds.GetRasterBand(1).SetNoDataValue(1)
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has nodata value 1.000000,"
          " but input band has no data value 0.000000.\n",
      )

    in_ds = mem_drv.Create("", 1, 1, 1)
    in_ds.GetRasterBand(1).SetNoDataValue(1)
    ref_ds = mem_drv.Create("", 1, 1, 1)
    ref_ds.GetRasterBand(1).SetNoDataValue(1)
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    in_f = mem_drv.Create("", 1, 1, 1, gdal.GDT_Float32)
    in_f.GetRasterBand(1).SetNoDataValue(float("nan"))
    ref_f = mem_drv.Create("", 1, 1, 1, gdal.GDT_Float32)
    ref_f.GetRasterBand(1).SetNoDataValue(float("nan"))
    with gdal.Run(
        "raster", "compare", input=in_f, reference=ref_f, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    ref_f.GetRasterBand(1).SetNoDataValue(1)
    with gdal.Run(
        "raster", "compare", input=in_f, reference=ref_f, skip_binary=True
    ) as alg:
      self.assertNotEqual(alg["output-string"], "")

  def test_color_interpretation_and_mask(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    in_ds = mem_drv.Create("", 1, 1, 1)
    in_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand)
    ref_ds = mem_drv.Create("", 1, 1, 1)
    ref_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_RedBand)

    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    ref_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_GreenBand)
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has color interpretation Green,"
          " but input band has color interpretation Red\n",
      )

    in_m = mem_drv.Create("", 1, 1, 1)
    in_m.CreateMaskBand(gdal.GMF_PER_DATASET)
    ref_m = mem_drv.Create("", 1, 1, 1)
    ref_m.CreateMaskBand(gdal.GMF_PER_DATASET)
    with gdal.Run(
        "raster", "compare", input=in_m, reference=ref_m, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    ref_m.GetRasterBand(1).GetMaskBand().Fill(1)
    with gdal.Run(
        "raster", "compare", input=in_m, reference=ref_m, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "mask of band 1: pixels differing: 1\n"
          "mask of band 1: maximum pixel value difference: 1\n",
      )

  def test_overviews(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    in_ds = mem_drv.Create("", 2, 2, 1)
    in_ds.BuildOverviews("NEAR", [2])
    ref_ds = mem_drv.Create("", 2, 2, 1)
    ref_ds.BuildOverviews("NEAR", [2])

    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    ref_no_ovr = mem_drv.Create("", 2, 2, 1)
    with gdal.Run(
        "raster",
        "compare",
        input=in_ds,
        reference=ref_no_ovr,
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has 0 overview band(s), but input band has 1\n",
      )

    for opt in ("skip_overview", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=in_ds,
          reference=ref_no_ovr,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

    in_4 = mem_drv.Create("", 4, 4, 1)
    in_4.BuildOverviews("NEAR", [4])
    ref_4 = mem_drv.Create("", 4, 4, 1)
    ref_4.BuildOverviews("NEAR", [2])
    with gdal.Run(
        "raster", "compare", input=in_4, reference=ref_4, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band width is 2, but input band width is 1\n"
          "Reference band height is 2, but input band height is 1\n",
      )

  def test_metadata_rpc_and_geolocation(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    in_ds = mem_drv.Create("", 1, 1, 1)
    ref_ds = mem_drv.Create("", 1, 1, 1)

    in_ds.SetMetadata({
        "ERR_BIAS": "foo",
        "one": "1",
        "two": "2",
        "three": "3",
        "NITF_FDT": "foo",
    })
    ref_ds.SetMetadata({
        "ERR_RAND": "foo",
        "two": "2",
        "three": "three",
        "four": "4",
        "NITF_FDT": "bar",
    })
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_ds, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference metadata (dataset default metadata domain) contains key"
          " 'four' but input metadata does not.\n"
          "Reference metadata (dataset default metadata domain) has value '3'"
          " for key 'three' but input metadata has value 'three'.\n"
          "Input metadata (dataset default metadata domain) contains key"
          " 'one' but reference metadata does not.\n",
      )

    for opt in ("skip_metadata", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=in_ds,
          reference=ref_ds,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

    # RPC strips leading and trailing spaces.
    in_rpc = mem_drv.Create("", 1, 1, 1)
    ref_rpc = mem_drv.Create("", 1, 1, 1)
    in_rpc.SetMetadata({"foo": "bar", "bar": "baz"}, "RPC")
    ref_rpc.SetMetadata({"foo": " bar ", "bar": "baw"}, "RPC")
    with gdal.Run(
        "raster", "compare", input=in_rpc, reference=ref_rpc, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference metadata RPC has value 'baz' for key 'bar' but input"
          " metadata has value 'baw'.\n",
      )
    for opt in ("skip_rpc", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=in_rpc,
          reference=ref_rpc,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

    # Geolocation domain.
    in_geo = mem_drv.Create("", 1, 1, 1)
    ref_geo = mem_drv.Create("", 1, 1, 1)
    in_geo.SetMetadata({"foo": "bar"}, "GEOLOCATION")
    ref_geo.SetMetadata({"bar": "baz"}, "GEOLOCATION")
    with gdal.Run(
        "raster", "compare", input=in_geo, reference=ref_geo, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference metadata GEOLOCATION contains key 'bar' but input"
          " metadata does not.\n"
          "Input metadata GEOLOCATION contains key 'foo' but reference"
          " metadata does not.\n",
      )
    for opt in ("skip_geolocation", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=in_geo,
          reference=ref_geo,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

  def test_subdataset_and_progress(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    uint16_tif = utilities_util.get_testdata_path("../gcore/data/uint16.tif")
    mem_drv = gdal.GetDriverByName("MEM")

    in_ds = mem_drv.Create("", 1, 1, 0)
    in_ds.SetMetadataItem("SUBDATASET_1_DESC", "FOO", "SUBDATASETS")
    in_ds.SetMetadataItem("SUBDATASET_1_NAME", byte_tif, "SUBDATASETS")
    ref_empty = mem_drv.Create("", 1, 1, 0)

    with gdal.Run(
        "raster",
        "compare",
        input=in_ds,
        reference=ref_empty,
        skip_binary=True,
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference dataset has 0 subdataset(s) whereas input dataset has 1"
          " one(s).\n",
      )

    ref_same = mem_drv.Create("", 1, 1, 0)
    ref_same.SetMetadataItem("SUBDATASET_1_DESC", "FOO", "SUBDATASETS")
    ref_same.SetMetadataItem("SUBDATASET_1_NAME", byte_tif, "SUBDATASETS")
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_same, skip_binary=True
    ) as alg:
      self.assertEqual(alg["output-string"], "")

    ref_diff = mem_drv.Create("", 1, 1, 0)
    ref_diff.SetMetadataItem("SUBDATASET_1_DESC", "FOO", "SUBDATASETS")
    ref_diff.SetMetadataItem("SUBDATASET_1_NAME", uint16_tif, "SUBDATASETS")
    with gdal.Run(
        "raster", "compare", input=in_ds, reference=ref_diff, skip_binary=True
    ) as alg:
      self.assertEqual(
          alg["output-string"],
          "Reference band 1 has data type UInt16,"
          " but input band has data type Byte\n",
      )

    for opt in ("skip_subdataset", "skip_all_optional"):
      with gdal.Run(
          "raster",
          "compare",
          input=in_ds,
          reference=ref_diff,
          skip_binary=True,
          **{opt: True},
      ) as alg:
        self.assertEqual(alg["output-string"], "")

    tab_pct = [0.0]

    def ok_progress(pct: float, _msg: str, _user_data: object) -> bool:
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    with gdal.Run(
        "raster",
        "compare",
        input=in_ds,
        reference=in_ds,
        skip_binary=True,
        progress=ok_progress,
    ) as alg:
      self.assertEqual(alg["output-string"], "")
    self.assertEqual(tab_pct[0], 1.0)


if __name__ == "__main__":
  absltest.main()
