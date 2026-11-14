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

"""Tests for `gdal raster nodata-to-alpha`.

Ported from upstream autotest/utilities/test_gdalalg_raster_nodata_to_alpha.py.

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


class GdalalgRasterNodataToAlphaTest(gdaltest.TestCase):

  def test_noop_without_nodata(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run(
        "raster", "nodata-to-alpha", input=byte_tif, output_format="MEM"
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 1)
      self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 4672)
      self.assertEqual(
          out_ds.GetRasterBand(1).GetMaskFlags(), gdal.GMF_ALL_VALID
      )

  def test_nominal_nodata(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
    src_ds.GetRasterBand(1).SetNoDataValue(1)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\x02")

    with gdal.Run(
        "raster", "nodata-to-alpha", input=src_ds, output_format="MEM"
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 2)
      self.assertEqual(
          out_ds.GetRasterBand(1).ReadRaster(),
          src_ds.GetRasterBand(1).ReadRaster(),
      )
      self.assertEqual(
          out_ds.GetRasterBand(1).GetMaskFlags(),
          gdal.GMF_ALPHA | gdal.GMF_PER_DATASET,
      )
      self.assertEqual(out_ds.GetRasterBand(2).ReadRaster(), b"\x00\xFF")
      self.assertEqual(
          out_ds.GetRasterBand(2).GetMaskFlags(), gdal.GMF_ALL_VALID
      )

  def test_override_nodata_single_value(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\x02")

    with gdal.Run(
        "raster",
        "nodata-to-alpha",
        input=src_ds,
        output_format="MEM",
        nodata=[1],
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 2)
      self.assertEqual(
          out_ds.GetRasterBand(1).ReadRaster(),
          src_ds.GetRasterBand(1).ReadRaster(),
      )
      self.assertEqual(
          out_ds.GetRasterBand(1).GetMaskFlags(),
          gdal.GMF_ALPHA | gdal.GMF_PER_DATASET,
      )
      self.assertEqual(out_ds.GetRasterBand(2).ReadRaster(), b"\x00\xFF")
      self.assertEqual(
          out_ds.GetRasterBand(2).GetMaskFlags(), gdal.GMF_ALL_VALID
      )

  def test_override_nodata_several_values(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 3, 1, 3)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 3, 1, b"\x01\x02\x03")
    src_ds.GetRasterBand(2).WriteRaster(0, 0, 3, 1, b"\x03\x01\x02")
    src_ds.GetRasterBand(3).WriteRaster(0, 0, 3, 1, b"\x03\x02\x01")

    with gdal.Run(
        "raster",
        "nodata-to-alpha",
        input=src_ds,
        output_format="MEM",
        nodata=[3, 2, 1],
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.RasterCount, 4)
      for band_idx in (1, 2, 3):
        self.assertEqual(
            out_ds.GetRasterBand(band_idx).ReadRaster(),
            src_ds.GetRasterBand(band_idx).ReadRaster(),
        )
        self.assertEqual(
            out_ds.GetRasterBand(band_idx).GetMaskFlags(),
            gdal.GMF_ALPHA | gdal.GMF_PER_DATASET,
        )
      self.assertEqual(out_ds.GetRasterBand(4).ReadRaster(), b"\xFF\xFF\x00")
      self.assertEqual(
          out_ds.GetRasterBand(4).GetMaskFlags(), gdal.GMF_ALL_VALID
      )

    with self.assertRaisesRegex(
        RuntimeError,
        "There should be 3 nodata values given the input dataset has 3 bands",
    ):
      gdal.Run(
          "raster",
          "nodata-to-alpha",
          input=src_ds,
          output_format="MEM",
          nodata=[3, 2],
      )


if __name__ == "__main__":
  absltest.main()
