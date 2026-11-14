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

"""Tests for `gdal raster mosaic`.

Ported from upstream autotest/utilities/test_gdalalg_raster_mosaic.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence
import os
import struct

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_mosaic_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["mosaic"]


class GdalalgRasterMosaicTest(gdaltest.TestCase, parameterized.TestCase):

  def _assert_geotransform_almost_equal(
      self,
      actual: Sequence[float],
      expected: Sequence[float],
  ) -> None:
    self.assertLen(actual, 6)
    self.assertLen(expected, 6)
    for a, e in zip(actual, expected):
      self.assertAlmostEqual(a, e)

  def test_from_dataset_handle(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_mosaic_alg()
    alg["input"] = [
        gdal.Translate("", byte_tif, options="-f MEM -srcwin 0 0 10 20"),
        gdal.Translate("", byte_tif, options="-f MEM -srcwin 10 0 10 20"),
    ]
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 20)
    self.assertEqual(ds.RasterYSize, 20)
    self._assert_geotransform_almost_equal(
        ds.GetGeoTransform(), (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0)
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_from_dataset_name(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_mosaic_alg()
    alg["input"] = [byte_tif]
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 20)
    self.assertEqual(ds.RasterYSize, 20)
    self._assert_geotransform_almost_equal(
        ds.GetGeoTransform(), (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0)
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_overwrite(self) -> None:
    utmsmall_tif = utilities_util.get_testdata_path("data/utmsmall.tif")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_mosaic_overwrite") as tmp:
      out_filename = f"{tmp}/out.vrt"

      alg = _get_mosaic_alg()
      self.assertTrue(alg.ParseRunAndFinalize([utmsmall_tif, out_filename]))

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)

      alg = _get_mosaic_alg()
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.ParseRunAndFinalize([byte_tif, out_filename])

      alg = _get_mosaic_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(["--overwrite", byte_tif, out_filename])
      )

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_bbox(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_mosaic_alg()
    self.assertTrue(
        alg.ParseCommandLineArguments([
            "--bbox=440780.0,3750180.0,441860.0,3751260.0",
            byte_tif,
            "",
            "--of=stream",
        ])
    )
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 18)
    self.assertEqual(ds.RasterYSize, 18)
    self._assert_geotransform_almost_equal(
        ds.GetGeoTransform(), (440780.0, 60.0, 0.0, 3751260.0, 0.0, -60.0)
    )
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 3695)

  @parameterized.parameters(
      ("average", 3, 1, (2.0, 0.75, 0.0, 49.0, 0.0, -0.75)),
      ("highest", 4, 2, (2.0, 0.5, 0.0, 49.0, 0.0, -0.5)),
      ("lowest", 2, 1, (2.0, 1.0, 0.0, 49.0, 0.0, -1.0)),
      ("0.5,1", 4, 1, (2.0, 0.5, 0.0, 49.0, 0.0, -1.0)),
  )
  def test_resolution_modes(
      self,
      resolution: str,
      expected_x: int,
      expected_y: int,
      expected_gt: tuple[float, ...],
  ) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 1, 1)
    src1_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src2_ds = mem_drv.Create("", 2, 2)
    src2_ds.SetGeoTransform([3, 0.5, 0, 49, 0, -0.5])

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds, src2_ds]
    alg["output-format"] = "stream"
    alg["resolution"] = resolution
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, expected_x)
    self.assertEqual(ds.RasterYSize, expected_y)
    self._assert_geotransform_almost_equal(ds.GetGeoTransform(), expected_gt)

  def test_resolution_common(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 5, 5)
    src1_ds.SetGeoTransform([2, 3, 0, 49, 0, -3])

    src2_ds = mem_drv.Create("", 3, 3)
    src2_ds.SetGeoTransform([17, 5, 0, 49, 0, -5])

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds, src2_ds]
    alg["output-format"] = "stream"
    alg["resolution"] = "common"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 30)
    self.assertEqual(ds.RasterYSize, 15)
    self._assert_geotransform_almost_equal(
        ds.GetGeoTransform(), (2.0, 1.0, 0.0, 49.0, 0.0, -1.0)
    )

  def test_target_aligned_pixels(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 1, 1)
    src1_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src2_ds = mem_drv.Create("", 2, 2)
    src2_ds.SetGeoTransform([3, 0.5, 0, 49, 0, -0.5])

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds, src2_ds]
    alg["output-format"] = "stream"
    alg["resolution"] = "0.3,0.6"
    alg["target-aligned-pixels"] = True
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 8)
    self.assertEqual(ds.RasterYSize, 2)
    self._assert_geotransform_almost_equal(
        ds.GetGeoTransform(), (1.8, 0.3, 0.0, 49.2, 0.0, -0.6)
    )

  def test_resolution_same_default(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 1, 1)
    src1_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src2_ds = mem_drv.Create("", 2, 2)
    src2_ds.SetGeoTransform([3, 0.5, 0, 49, 0, -0.5])

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds, src2_ds]
    alg["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError, "whereas previous sources have resolution"
    ):
      alg.Run()

  def test_resolution_invalid(self) -> None:
    alg = _get_mosaic_alg()
    msg = (
        "resolution: two comma separated positive values should be provided,"
        " or "
    )
    for bad in ("invalid", "0.5", "-0.5,-0.5"):
      with self.assertRaisesRegex(RuntimeError, msg):
        alg["resolution"] = bad

  def test_srcnodata_dstnodata(self) -> None:
    src1_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src1_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src1_ds.GetRasterBand(1).Fill(1)

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds]
    alg["output-format"] = "stream"
    alg["src-nodata"] = [1]
    alg["dst-nodata"] = [2]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 2)
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 2)

  def test_hidenodata(self) -> None:
    src1_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src1_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src1_ds.GetRasterBand(1).Fill(1)

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds]
    alg["output-format"] = "stream"
    alg["src-nodata"] = [1]
    alg["dst-nodata"] = [2]
    alg["hide-nodata"] = True
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 2)
    self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())

  def test_band_selection(self) -> None:
    rgbsmall_tif = utilities_util.get_testdata_path(
        "../gcore/data/rgbsmall.tif"
    )
    alg = _get_mosaic_alg()
    alg["input"] = [rgbsmall_tif]
    alg["output-format"] = "stream"
    alg["band"] = [3, 2]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterCount, 2)
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 21349)
    self.assertEqual(ds.GetRasterBand(2).Checksum(), 21053)

  def test_glob(self) -> None:
    rgbsmall_tif = utilities_util.get_testdata_path(
        "../gcore/data/rgbsmall.tif"
    )
    pattern = os.path.join(os.path.dirname(rgbsmall_tif), "rgbsm?ll.tif")
    alg = _get_mosaic_alg()
    alg["input"] = [pattern]
    alg["output-format"] = "stream"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterCount, 3)

  def test_at_filename(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_mosaic_at_file") as tmp:
      input_file_list = f"{tmp}/tmp.txt"
      gdal.FileFromMemBuffer(input_file_list, byte_tif)

      alg = _get_mosaic_alg()
      alg["input"] = [f"@{input_file_list}"]
      alg["output-format"] = "stream"
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_at_filename_error(self) -> None:
    alg = _get_mosaic_alg()
    alg["input"] = ["@i_do_not_exist"]
    alg["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError, "mosaic: Cannot open i_do_not_exist"
    ):
      alg.Run()

  def test_co(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_mosaic_co") as tmp:
      out_tif = f"{tmp}/out.tif"
      alg = _get_mosaic_alg()
      alg["input"] = [byte_tif]
      alg["output"] = out_tif
      alg["creation-option"] = ["BLOCKYSIZE=10"]
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_tif) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [20, 10])

  def test_tif_output_implicit_and_explicit(self) -> None:
    utmsmall_tif = utilities_util.get_testdata_path("data/utmsmall.tif")
    with utilities_util.vsimem_dir("raster_mosaic_tif_out") as tmp:
      out_tif = f"{tmp}/out.tif"
      alg = _get_mosaic_alg()
      self.assertTrue(alg.ParseRunAndFinalize([utmsmall_tif, out_tif]))
      with gdal.Open(out_tif) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)

      out_xxx = f"{tmp}/out.xxx"
      alg = _get_mosaic_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(["--of=GTiff", utmsmall_tif, out_xxx])
      )
      with gdal.Open(out_xxx) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)

      out_tiled = f"{tmp}/out_tiled.xxx"
      alg = _get_mosaic_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(
              ["--of=GTiff", "--co=TILED=YES", utmsmall_tif, out_tiled]
          )
      )
      with gdal.Open(out_tiled) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)
        self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [256, 256])

  def test_inconsistent_characteristics(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 1, 1)
    src1_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src2_ds = mem_drv.Create("", 2, 2)
    src2_ds.SetGeoTransform([3, 0.5, 0, 49, 0, -0.5])
    srs = osr.SpatialReference()
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    srs.SetFromUserInput("WGS84")
    src2_ds.SetSpatialRef(srs)

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds, src2_ds]
    alg["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError,
        "gdal raster mosaic does not support heterogeneous projection",
    ):
      alg.Run()

  def test_absolute_path(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_mosaic_abspath") as tmp:
      tmp_byte = f"{tmp}/byte.tif"
      out_filename = f"{tmp}/out.vrt"
      gdal.Translate(tmp_byte, byte_tif)

      gdal.Run(
          "raster",
          "mosaic",
          input=tmp_byte,
          output=out_filename,
          absolute_path=True,
      )

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      content = gcore_util.vsi_read_file_as_string(out_filename, 65536)
      self.assertIn(f'<SourceFilename relativeToVRT="0">{tmp_byte}', content)
      self.assertNotIn('<SourceFilename relativeToVRT="1">byte.tif', content)

  @parameterized.parameters(
      ("sum", ["k=4"], (5, 7, 7, 6)),
      ("min", [], (1, 1, 1, 2)),
  )
  def test_pixel_function(
      self,
      pixfn: str,
      args: list[str],
      expected: tuple[int, ...],
  ) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 3, 1, eType=gdal.GDT_Int16)
    src1_ds.SetGeoTransform([0, 1, 0, 1, 0, -1])
    src1_ds.GetRasterBand(1).Fill(1)

    src2_ds = mem_drv.Create("", 3, 1, eType=gdal.GDT_Int16)
    src2_ds.SetGeoTransform([1, 1, 0, 1, 0, -1])
    src2_ds.GetRasterBand(1).Fill(2)

    alg = _get_mosaic_alg()
    alg["input"] = [src1_ds, src2_ds]
    alg["output-format"] = "stream"
    alg["dst-nodata"] = [-999]
    alg["pixel-function"] = pixfn
    alg["pixel-function-arg"] = args
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")

    raw = ds.GetRasterBand(1).ReadRaster()
    self.assertEqual(struct.unpack("4h", raw), expected)
    self.assertTrue(alg.Finalize())

  def test_pixel_function_invalid(self) -> None:
    alg = _get_mosaic_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Invalid value 'does_not_exist' for string argument 'pixel-function'",
    ):
      alg["pixel-function"] = "does_not_exist"

  def test_pixel_function_arg_invalid(self) -> None:
    alg = _get_mosaic_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Invalid value for argument 'pixel-function-arg'."
        " <KEY>=<VALUE> expected",
    ):
      alg["pixel-function-arg"] = "key_without_value"

  def test_pixel_function_arg_complete(self) -> None:
    base = ["completion", "gdal", "raster", "mosaic"]

    out, _, _ = utilities_util.run_gdal_cli([*base, "--pixel-function-arg"])
    self.assertIn(
        "Specify argument(s) to pass to the pixel function".replace(" ", "\\ "),
        out,
    )

    out, _, _ = utilities_util.run_gdal_cli(
        [*base, "--pixel-function=invalid", "--pixel-function-arg"]
    )
    self.assertIn("Invalid pixel function name".replace(" ", "\\ "), out)

    out, _, _ = utilities_util.run_gdal_cli(
        [*base, "--pixel-function=scale", "--pixel-function-arg"]
    )
    self.assertIn(
        "No pixel function arguments for pixel function".replace(" ", "\\ "),
        out,
    )

    out, _, _ = utilities_util.run_gdal_cli(
        [*base, "--pixel-function=mean", "--pixel-function-arg"]
    )
    self.assertEqual(out, "propagateNoData=")

    out, _, _ = utilities_util.run_gdal_cli([
        *base,
        "--pixel-function=mean",
        "--pixel-function-arg",
        "propagateNoData=",
    ])
    self.assertEqual(out.split(" "), ["NO", "YES"])

    out, _, _ = utilities_util.run_gdal_cli([
        *base,
        "--pixel-function=mean",
        "--pixel-function-arg",
        "propagateNoData=YES",
    ])
    self.assertEqual(out, "")

  def test_pipeline(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src1_ds = mem_drv.Create("", 3, 1, eType=gdal.GDT_Int16)
    src1_ds.SetGeoTransform([0, 1, 0, 1, 0, -1])
    src1_ds.GetRasterBand(1).Fill(1)

    src2_ds = mem_drv.Create("", 3, 1, eType=gdal.GDT_Int16)
    src2_ds.SetGeoTransform([1, 1, 0, 1, 0, -1])
    src2_ds.GetRasterBand(1).Fill(2)

    with gdal.Run(
        "raster",
        "pipeline",
        pipeline="mosaic ! write --of=stream streamed_output",
        input=[src1_ds, src2_ds],
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 7)


if __name__ == "__main__":
  absltest.main()
