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

"""Tests for `gdal raster clean-collar`.

Ported from upstream autotest/utilities/test_gdalalg_raster_clean_collar.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["clean-collar"]


class GdalalgRasterCleanCollarTest(gdaltest.TestCase):

  def test_progress_and_basic(self) -> None:
    last_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: object) -> bool:
      del msg, user_data
      last_pct[0] = pct
      return True

    alg = _get_alg()
    alg["input"] = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run(my_progress))
    self.assertEqual(last_pct[0], 1.0)
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterCount, 1)
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_update_output(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_clean_collar_out") as tmp:
      out_filename = f"{tmp}/out.tif"

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      ds = None
      self.assertTrue(alg.Finalize())

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["creation-option"] = {"COMPRESS": "LZW"}
      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_filename, gdal.GA_Update) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )
        ds.GetRasterBand(1).Fill(0)

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["update"] = True
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_update_input_as_object(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
    ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x02\xFF")

    alg = _get_alg()
    alg["input"] = ds
    with self.assertRaisesRegex(
        RuntimeError,
        "Output dataset is not specified. If you intend to update the input"
        " dataset, set the 'update' option",
    ):
      alg.Run()

    alg = _get_alg()
    alg["input"] = ds
    alg["update"] = True
    self.assertTrue(alg.Run())
    self.assertEqual(ds.GetRasterBand(1).ReadRaster(), b"\x00\xFF")

  def test_update_input_as_name(self) -> None:
    with utilities_util.vsimem_dir("raster_clean_collar_name") as tmp:
      filename = f"{tmp}/test.tif"
      ds = gdal.GetDriverByName("GTiff").Create(filename, 2, 1)
      ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x02\xFF")
      ds.Close()

      alg = _get_alg()
      alg["input"] = filename
      with self.assertRaisesRegex(
          RuntimeError,
          "Output dataset is not specified. If you intend to update the input"
          " dataset, set the 'update' option",
      ):
        alg.Run()

      alg = _get_alg()
      alg["input"] = filename
      alg["update"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetRasterBand(1).ReadRaster(), b"\x00\xFF")
      ds = None
      self.assertTrue(alg.Finalize())

  def test_color_and_pixel_distance(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 4, 1)
    ds.GetRasterBand(1).WriteRaster(0, 0, 4, 1, b"\x00\x7F\xD0\xFF")

    alg = _get_alg()
    alg["input"] = ds
    alg["update"] = True
    alg["color"] = ["black", "white", 0xD0]
    alg["pixel-distance"] = 0
    self.assertTrue(alg.Run())
    self.assertEqual(ds.GetRasterBand(1).ReadRaster(), b"\x00\x7F\x00\x00")

    alg = _get_alg()
    with self.assertRaisesRegex(RuntimeError, "Value for 'color' should be"):
      alg["color"] = "invalid"

  def test_color_tuple(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 4, 1, 2)
    ds.GetRasterBand(1).WriteRaster(0, 0, 4, 1, b"\x00\x00\x01\xFF")
    ds.GetRasterBand(2).WriteRaster(0, 0, 4, 1, b"\x01\x00\x01\xFE")

    alg = _get_alg()
    alg["input"] = ds
    alg["update"] = True
    alg["color"] = ["0,1"]
    alg["pixel-distance"] = 0
    alg["color-threshold"] = 0
    self.assertTrue(alg.Run())
    self.assertEqual(ds.GetRasterBand(1).ReadRaster(), b"\x00\x00\x01\xFF")
    self.assertEqual(ds.GetRasterBand(2).ReadRaster(), b"\x00\x00\x01\xFE")

  def test_color_threshold(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 4, 1)
    ds.GetRasterBand(1).WriteRaster(0, 0, 4, 1, b"\x00\x01\x02\x00")

    alg = _get_alg()
    alg["input"] = ds
    alg["update"] = True
    alg["color"] = ["black", "white", 0xD0]
    alg["pixel-distance"] = 0
    alg["color-threshold"] = 1
    self.assertTrue(alg.Run())
    self.assertEqual(ds.GetRasterBand(1).ReadRaster(), b"\x00\x00\x02\x00")

  def test_add_alpha(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
    ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\xFE")

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = "my_name"
    alg["output-format"] = "MEM"
    alg["add-alpha"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.RasterCount, 2)
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\xFE")
    self.assertEqual(out_ds.GetRasterBand(2).ReadRaster(), b"\x00\xFF")
    self.assertEqual(
        out_ds.GetRasterBand(2).GetColorInterpretation(), gdal.GCI_AlphaBand
    )

    out_ds.GetRasterBand(2).Fill(0x7F)

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = out_ds
    alg["update"] = True
    self.assertTrue(alg.Run())
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\xFE")
    self.assertEqual(out_ds.GetRasterBand(2).ReadRaster(), b"\x00\xFF")

  def test_add_mask(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
    ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\xFE")

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = "my_name"
    alg["output-format"] = "MEM"
    alg["add-mask"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.RasterCount, 1)
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\xFE")
    self.assertEqual(
        out_ds.GetRasterBand(1).GetMaskBand().ReadRaster(), b"\x00\xFF"
    )

    out_ds.GetRasterBand(1).GetMaskBand().Fill(0x7F)

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = out_ds
    alg["update"] = True
    self.assertTrue(alg.Run())
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\xFE")
    self.assertEqual(
        out_ds.GetRasterBand(1).GetMaskBand().ReadRaster(), b"\x00\xFF"
    )

  def test_add_mask_from_alpha(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 2, 1, 2)
    ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\xFE")
    ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_AlphaBand)
    ds.GetRasterBand(2).WriteRaster(0, 0, 2, 1, b"\xFF\xFF")

    alg = _get_alg()
    alg["input"] = ds
    alg["output"] = "my_name"
    alg["output-format"] = "MEM"
    alg["add-mask"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.RasterCount, 1)
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\xFE")
    self.assertEqual(
        out_ds.GetRasterBand(1).GetMaskBand().ReadRaster(), b"\x00\xFF"
    )

  def test_algorithm_default_is_floodfill(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 4, 4)
    ds.GetRasterBand(1).WriteRaster(0, 0, 4, 1, b"\x00\xFF\xFF\xFF")
    ds.GetRasterBand(1).WriteRaster(0, 1, 4, 1, b"\x00\xFF\x01\xFF")
    ds.GetRasterBand(1).WriteRaster(0, 2, 4, 1, b"\x00\x00\x01\xFF")
    ds.GetRasterBand(1).WriteRaster(0, 3, 4, 1, b"\x00\xFF\xFF\xFF")

    alg = _get_alg()
    alg["input"] = ds
    alg["update"] = True
    alg["pixel-distance"] = 0
    self.assertTrue(alg.Run())

    self.assertEqual(ds.ReadRaster(0, 0, 4, 1), b"\x00\xFF\xFF\xFF")
    self.assertEqual(ds.ReadRaster(0, 1, 4, 1), b"\x00\xFF\x00\xFF")
    self.assertEqual(ds.ReadRaster(0, 2, 4, 1), b"\x00\x00\x00\xFF")
    self.assertEqual(ds.ReadRaster(0, 3, 4, 1), b"\x00\xFF\xFF\xFF")

  def test_algorithm_twopasses(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 4, 4)
    ds.GetRasterBand(1).WriteRaster(0, 0, 4, 1, b"\x00\xFF\xFF\xFF")
    ds.GetRasterBand(1).WriteRaster(0, 1, 4, 1, b"\x00\xFF\x01\xFF")
    ds.GetRasterBand(1).WriteRaster(0, 2, 4, 1, b"\x00\x00\x01\xFF")
    ds.GetRasterBand(1).WriteRaster(0, 3, 4, 1, b"\x00\xFF\xFF\xFF")

    alg = _get_alg()
    alg["input"] = ds
    alg["update"] = True
    alg["pixel-distance"] = 0
    alg["algorithm"] = "twopasses"
    self.assertTrue(alg.Run())

    self.assertEqual(ds.ReadRaster(0, 0, 4, 1), b"\x00\xFF\xFF\xFF")
    self.assertEqual(ds.ReadRaster(0, 1, 4, 1), b"\x00\xFF\x01\xFF")
    self.assertEqual(ds.ReadRaster(0, 2, 4, 1), b"\x00\x00\x00\xFF")
    self.assertEqual(ds.ReadRaster(0, 3, 4, 1), b"\x00\xFF\xFF\xFF")


if __name__ == "__main__":
  absltest.main()
