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

"""Tests for `gdal raster update`.

Ported from upstream autotest/utilities/test_gdalalg_raster_update.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


class GdalalgRasterUpdateTest(gdaltest.TestCase, parameterized.TestCase):

  def test_in_mem(self) -> None:
    out_ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
    out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.SetGeoTransform([1, 1, 0, -1, 0, -1])
    src_ds.GetRasterBand(1).Fill(1)

    self.assertTrue(
        gdal.Run("gdal", "raster", "update", input=src_ds, output=out_ds)
    )
    data = out_ds.ReadRaster(0, 0, 3, 3)
    self.assertIsNotNone(data)
    assert data is not None
    self.assertEqual(list(data), [0, 0, 0, 0, 1, 0, 0, 0, 0])

  def test_gtiff(self) -> None:
    with utilities_util.vsimem_dir("raster_update_gtiff") as tmp:
      dst_filename = f"{tmp}/dst.tif"
      with gdal.GetDriverByName("GTiff").Create(dst_filename, 3, 3) as out_ds:
        out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

      src_filename = f"{tmp}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(src_filename, 1, 1) as src_ds:
        src_ds.SetGeoTransform([1, 1, 0, -1, 0, -1])
        src_ds.GetRasterBand(1).Fill(1)

      tab_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        self.assertGreaterEqual(pct, tab_pct[0])
        tab_pct[0] = pct
        return True

      with gdal.Run(
          "gdal",
          "raster",
          "update",
          input=src_filename,
          output=dst_filename,
          progress=my_progress,
          resampling="cubic",
          warp_option={"SKIP_NOSOURCE": "YES"},
          transform_option={"SRC_METHOD": "GEOTRANSFORM"},
          error_threshold=0,
      ) as alg:
        ret_ds = alg.Output()
        data = ret_ds.ReadRaster(0, 0, 3, 3)
        self.assertIsNotNone(data)
        assert data is not None
        self.assertEqual(list(data), [0, 0, 0, 0, 1, 0, 0, 0, 0])

      self.assertEqual(tab_pct[0], 1.0)

  def test_src_is_same_as_dst_pointer(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
    ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    with self.assertRaisesRegex(
        RuntimeError, "Source and destination datasets must be different"
    ):
      gdal.Run("gdal", "raster", "update", input=ds, output=ds)

  def test_src_is_same_as_dst_filename(self) -> None:
    with utilities_util.vsimem_dir("raster_update_same") as tmp:
      filename = f"{tmp}/tmp.tif"
      with gdal.GetDriverByName("GTiff").Create(filename, 3, 3) as ds:
        ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

      with gdal.Open(filename) as src_ds:
        with gdal.Open(filename, gdal.GA_Update) as dst_ds:
          with self.assertRaisesRegex(
              RuntimeError, "Source and destination datasets must be different"
          ):
            gdal.Run("gdal", "raster", "update", input=src_ds, output=dst_ds)

  def test_dst_does_not_exist(self) -> None:
    ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
    ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    with self.assertRaisesRegex(RuntimeError, "/i_do/not/exist"):
      gdal.Run("gdal", "raster", "update", input=ds, output="/i_do/not/exist")

  def test_geometry_and_invalid(self) -> None:
    out_ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
    out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    src_ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
    src_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    src_ds.GetRasterBand(1).Fill(1)

    self.assertTrue(
        gdal.Run(
            "gdal",
            "raster",
            "update",
            input=src_ds,
            output=out_ds,
            geometry="POLYGON ((1 -1,2 -1,2 -2,1 -2,1 -1))",
        )
    )
    data = out_ds.ReadRaster(0, 0, 3, 3)
    self.assertIsNotNone(data)
    assert data is not None
    self.assertEqual(list(data), [0, 0, 0, 0, 1, 0, 0, 0, 0])

    with self.assertRaisesRegex(
        RuntimeError,
        "Clipping geometry is neither a valid WKT or GeoJSON geometry",
    ):
      gdal.Run(
          "gdal",
          "raster",
          "update",
          input=src_ds,
          output=out_ds,
          geometry="invalid",
      )

  @parameterized.parameters(True, False)
  def test_refresh_overviews(self, with_crs: bool) -> None:
    crs = osr.SpatialReference(epsg=32631)

    out_ds = gdal.GetDriverByName("MEM").Create("", 1024, 1024)
    out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    out_ds.BuildOverviews("NEAR", [2])
    out_ds.GetRasterBand(1).GetOverview(0).Fill(127)
    if with_crs:
      out_ds.SetSpatialRef(crs)

    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    src_ds.SetGeoTransform([511, 1, 0, -511, 0, -1])
    src_ds.GetRasterBand(1).Fill(255)
    if with_crs:
      src_ds.SetSpatialRef(crs)

    tab_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    self.assertTrue(
        gdal.Run(
            "gdal",
            "raster",
            "update",
            input=src_ds,
            output=out_ds,
            resampling="cubic",
            progress=my_progress,
        )
    )
    self.assertEqual(tab_pct[0], 1.0)

    data = out_ds.ReadRaster(511, 511, 2, 2)
    self.assertIsNotNone(data)
    assert data is not None
    self.assertEqual(list(data), [255, 255, 255, 255])

    ovr_data = out_ds.GetRasterBand(1).GetOverview(0).ReadRaster(254, 254, 4, 4)
    self.assertIsNotNone(ovr_data)
    assert ovr_data is not None
    self.assertEqual(
        list(ovr_data),
        [
            127,
            127,
            127,
            127,
            127,
            76,
            76,
            127,
            127,
            76,
            76,
            127,
            127,
            127,
            127,
            127,
        ],
    )

  def test_cannot_update_overviews(self) -> None:
    out_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    out_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    out_ds.SetSpatialRef(osr.SpatialReference(epsg=32631))
    out_ds.BuildOverviews("NEAR", [2])

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.SetGeoTransform([1, 1, 0, -1, 0, -1])
    src_ds.SetSpatialRef(osr.SpatialReference("+proj=longlat +a=1"))

    errors: list[tuple[int, int, str]] = []

    def handler(err_type: int, err_no: int, err_msg: str) -> None:
      errors.append((err_type, err_no, err_msg))

    gdal.PushErrorHandler(handler)
    try:
      with self.assertRaises(RuntimeError):
        gdal.Run("gdal", "raster", "update", input=src_ds, output=out_ds)
    finally:
      gdal.PopErrorHandler()

    self.assertTrue(
        any(lvl == gdal.CE_Warning for lvl, _, _ in errors),
        msg=f"Expected CE_Warning, got {errors}",
    )


if __name__ == "__main__":
  absltest.main()
