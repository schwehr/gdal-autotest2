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

"""Tests for `gdal raster reclassify`.

Ported from upstream autotest/utilities/test_gdalalg_raster_reclassify.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

import random
import struct

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_reclassify_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["reclassify"]


class GdalalgRasterReclassifyTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      ("tif", "file"),
      ("tif", "text"),
      ("vrt", "file"),
      ("vrt", "text"),
  )
  def test_basic(self, output_format: str, mapping_format: str) -> None:
    nodata_tif = utilities_util.get_testdata_path(
        "../gcore/data/nodata_byte.tif"
    )
    if output_format == "vrt":
      src = gdal.Open(nodata_tif)
    else:
      src = gdal.Translate("", nodata_tif, format="MEM")
    self.assertIsNotNone(src)
    assert src is not None

    with utilities_util.vsimem_dir("raster_reclassify_basic") as tmp:
      outfile = f"{tmp}/out.{output_format}"
      alg = _get_reclassify_alg()
      alg["input"] = src
      alg["output"] = outfile

      if mapping_format == "text":
        alg["mapping"] = (
            "165 = 120; (-inf, 0) = 140; (0, 100] = 140;"
            " (100,  130] = PASS_THROUGH; DEFAULT = 160; NO_DATA = NO_DATA"
        )
      else:
        mapping_file = f"{tmp}/mapping.txt"
        gdal.FileFromMemBuffer(
            mapping_file,
            """
            # A sample reclassification
            165         = 120
            (-inf, 0)   = 140 # Match everything <= 100
            (0, 100]    = 140 # manually exclude NoData value of 0
            (100, 130]  = PASS_THROUGH
            DEFAULT     = 160
            NO_DATA     = NO_DATA
            """,
        )
        alg["mapping"] = f"@{mapping_file}"

      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(outfile) as dst:
        self.assertEqual(src.GetGeoTransform(), dst.GetGeoTransform())
        self.assertIsSame(src.GetSpatialRef(), dst.GetSpatialRef())
        src_band = src.GetRasterBand(1)
        dst_band = dst.GetRasterBand(1)
        nodata = src_band.GetNoDataValue()
        self.assertEqual(nodata, dst_band.GetNoDataValue())
        self.assertEqual(src_band.DataType, dst_band.DataType)

        src_vals = src_band.ReadRaster()
        dst_vals = dst_band.ReadRaster()
        for s, d in zip(src_vals, dst_vals):
          if s == nodata:
            self.assertEqual(d, nodata)
          elif s == 165:
            self.assertEqual(d, 120)
          elif s <= 100:
            self.assertEqual(d, 140)
          elif 100 < s <= 130:
            self.assertEqual(d, s)
          else:
            self.assertEqual(d, 160)

  def test_output_type(self) -> None:
    infile = utilities_util.get_testdata_path("../gcore/data/float32.tif")
    with utilities_util.vsimem_dir("raster_reclassify_type") as tmp:
      outfile = f"{tmp}/out.tif"
      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      alg["mapping"] = "(-inf, 132)=0; [132, inf)=1"
      alg["output-data-type"] = "Int16"
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(infile) as src, gdal.Open(outfile) as dst:
        dst_band = dst.GetRasterBand(1)
        self.assertEqual(dst_band.DataType, gdal.GDT_Int16)
        count = src.RasterXSize * src.RasterYSize
        src_vals = struct.unpack(f"{count}f", src.GetRasterBand(1).ReadRaster())
        dst_vals = struct.unpack(f"{count}h", dst_band.ReadRaster())
        for s, d in zip(src_vals, dst_vals):
          self.assertEqual(d, 0 if s < 132 else 1)

  def test_source_transfer_type(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 5, 5, eType=gdal.GDT_Int32)
    src_ds.GetRasterBand(1).Fill(500)

    alg = _get_reclassify_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["mapping"] = "[-inf, 500)=3; [500, inf]=7"
    alg["output-data-type"] = "Byte"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(list(out_ds.GetRasterBand(1).ReadRaster()), [7] * 25)
    self.assertTrue(alg.Finalize())

  def test_many_ranges(self) -> None:
    nx = 100
    ny = 100
    nbins = 1000

    rng = random.Random(802)
    data = [rng.random() * 1000.0 for _ in range(nx * ny)]

    src_ds = gdal.GetDriverByName("MEM").Create(
        "", nx, ny, 1, eType=gdal.GDT_Float64
    )
    src_ds.GetRasterBand(1).WriteRaster(
        0, 0, nx, ny, struct.pack(f"{nx * ny}d", *data)
    )
    rast_min, rast_max = src_ds.GetRasterBand(1).ComputeRasterMinMax(False)

    breaks = [
        rast_min + (rast_max - rast_min) * i / (nbins - 2)
        for i in range(nbins - 1)
    ]
    bins: dict[int, tuple[float, float]] = {}
    for idx, (a, b) in enumerate(zip(breaks, breaks[1:] + [float("inf")])):
      bins[idx] = (a, b)

    with utilities_util.vsimem_dir("raster_reclassify_many") as tmp:
      bins_path = f"{tmp}/bins.txt"
      with gdal.VSIFile(bins_path, "w") as out:
        for idx, (a, b) in bins.items():
          out.write(f"[{a}, {b}) = {idx}\n")

      alg = _get_reclassify_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["mapping"] = f"@{bins_path}"
      alg["output-format"] = "MEM"
      self.assertTrue(alg.Run())

      out_ds = utilities_util.get_arg_dataset(alg, "output")
      results = struct.unpack(
          f"{nx * ny}d", out_ds.GetRasterBand(1).ReadRaster()
      )
      self.assertTrue(alg.Finalize())

      for src_val, res_val in zip(data, results):
        a, b = bins[int(res_val)]
        self.assertGreaterEqual(src_val, a)
        self.assertLess(src_val, b)

  def test_multiple_bands(self) -> None:
    infile = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    with utilities_util.vsimem_dir("raster_reclassify_multiband") as tmp:
      outfile = f"{tmp}/out.tif"
      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      alg["mapping"] = "(-inf, 128)=0; [128, inf)=1"
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(infile) as src, gdal.Open(outfile) as dst:
        self.assertEqual(src.RasterCount, dst.RasterCount)
        for i in range(src.RasterCount):
          src_band = src.GetRasterBand(i + 1)
          dst_band = dst.GetRasterBand(i + 1)
          self.assertEqual(src_band.DataType, dst_band.DataType)
          for s, d in zip(src_band.ReadRaster(), dst_band.ReadRaster()):
            self.assertEqual(d, 0 if s < 128 else 1)

  def test_errors(self) -> None:
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_reclassify_err") as tmp:
      outfile = f"{tmp}/out.tif"

      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      alg["mapping"] = ""
      with self.assertRaisesRegex(
          RuntimeError, "Encountered value .* with no specified mapping"
      ):
        alg.Run()

      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = f"{tmp}/out.vrt"
      alg["mapping"] = "invalid"
      with self.assertRaisesRegex(RuntimeError, "Interval must start with"):
        alg.Run()

      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      alg["mapping"] = f"@{tmp}/i_do_not_exist.txt"
      with self.assertRaisesRegex(
          RuntimeError, "Cannot open .*i_do_not_exist.txt"
      ):
        alg.Run()

      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      with self.assertRaisesRegex(
          RuntimeError, "Required argument 'mapping' has not been specified"
      ):
        alg.Run()

      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      with self.assertRaisesRegex(
          RuntimeError, "Invalid value .*output-data-type"
      ):
        alg["output-data-type"] = "Float128"

      long_line_path = f"{tmp}/long_line.txt"
      gdal.FileFromMemBuffer(long_line_path, "x" * (1000 * 1000 + 1))
      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      alg["mapping"] = f"@{long_line_path}"
      with self.assertRaisesRegex(
          RuntimeError, "Maximum number of characters allowed reached"
      ):
        alg.Run()

      large_file_path = f"{tmp}/large_file.txt"
      gdal.FileFromMemBuffer(
          large_file_path,
          "\n".join(["x" * (1000 * 1000 - 1) for _ in range(10)])
          + "\n"
          + "x" * 10,
      )
      alg = _get_reclassify_alg()
      alg["input"] = infile
      alg["output"] = outfile
      alg["mapping"] = f"@{large_file_path}"
      with self.assertRaisesRegex(RuntimeError, "Too large mapping size"):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
