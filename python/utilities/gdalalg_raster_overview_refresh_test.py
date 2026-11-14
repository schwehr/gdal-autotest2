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

"""Tests for `gdal raster overview refresh`.

Ported from upstream autotest/utilities/test_gdalalg_raster_overview_refresh.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import array
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_refresh_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["overview"]["refresh"]


def _vrt_has_open_support() -> bool:
  drv = gdal.GetDriverByName("VRT")
  return (
      drv is not None
      and drv.GetMetadataItem(gdal.DMD_OPENOPTIONLIST) is not None
  )


class GdalalgRasterOverviewRefreshTest(
    gdaltest.TestCase, parameterized.TestCase
):

  def test_no_option(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    ds = gdal.Translate(
        "", byte_tif, options="-f MEM -outsize 512 512 -r cubic"
    )
    ds.BuildOverviews("BILINEAR", [2])
    ovr_data_ori = array.array(
        "B", ds.GetRasterBand(1).GetOverview(0).ReadRaster()
    )
    ds.GetRasterBand(1).GetOverview(0).Fill(0)

    alg = _get_refresh_alg()
    alg["dataset"] = ds
    alg["resampling"] = "bilinear"
    self.assertTrue(alg.Run())

    ovr_band = ds.GetRasterBand(1).GetOverview(0)
    ovr_data_refreshed = array.array("B", ovr_band.ReadRaster())
    self.assertEqual(ovr_data_refreshed, ovr_data_ori)

  def test_bbox(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    ds = gdal.Translate(
        "", byte_tif, options="-f MEM -outsize 512 512 -r cubic"
    )
    ds.BuildOverviews("BILINEAR", [2, 4])
    ovr_data_ori = array.array(
        "B", ds.GetRasterBand(1).GetOverview(0).ReadRaster()
    )
    ds.GetRasterBand(1).Fill(0)
    gt = ds.GetGeoTransform()

    x, y, width, height = 10, 20, 30, 40
    ulx = gt[0] + gt[1] * x
    uly = gt[3] + gt[5] * y
    lrx = gt[0] + gt[1] * (x + width)
    lry = gt[3] + gt[5] * (y + height)

    alg = _get_refresh_alg()
    alg["dataset"] = ds
    alg["bbox"] = [ulx, lry, lrx, uly]
    self.assertTrue(alg.Run())

    ovr_band = ds.GetRasterBand(1).GetOverview(0)
    ovr_data_refreshed = array.array("B", ovr_band.ReadRaster())
    for j in range(height // 2):
      for i in range(width // 2):
        idx = (y // 2 + j) * ovr_band.XSize + (x // 2 + i)
        self.assertEqual(ovr_data_refreshed[idx], 0)
        ovr_data_refreshed[idx] = ovr_data_ori[idx]
    self.assertEqual(ovr_data_refreshed, ovr_data_ori)

  def test_bbox_gt_errors(self) -> None:
    ds_no_gt = gdal.GetDriverByName("MEM").Create("", 2, 2)
    ds_no_gt.BuildOverviews("NEAR", [2])
    alg = _get_refresh_alg()
    alg["dataset"] = ds_no_gt
    alg["bbox"] = [0, 0, 1, 1]
    with self.assertRaisesRegex(RuntimeError, "Dataset has no geotransform"):
      alg.Run()

    ds_bad_gt = gdal.GetDriverByName("MEM").Create("", 2, 2)
    ds_bad_gt.SetGeoTransform([0, 0, 0, 0, 0, 0])
    ds_bad_gt.BuildOverviews("NEAR", [2])
    alg2 = _get_refresh_alg()
    alg2["dataset"] = ds_bad_gt
    alg2["bbox"] = [0, 0, 1, 1]
    with self.assertRaisesRegex(RuntimeError, "Cannot invert geotransform"):
      alg2.Run()

  @parameterized.parameters(True, False)
  def test_like(self, with_progress: bool) -> None:
    if not _vrt_has_open_support():
      self.skipTest("VRT driver open missing")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      left_tif = os.path.join(tmpdir, "left.tif")
      right_tif = os.path.join(tmpdir, "right.tif")
      tmp_vrt = os.path.join(tmpdir, "tmp.vrt")

      gdal.Translate(left_tif, byte_tif, options="-srcwin 0 0 10 20")
      gdal.Translate(right_tif, byte_tif, options="-srcwin 10 0 10 20")
      ds = gdal.BuildVRT(tmp_vrt, [left_tif, right_tif])
      ds.BuildOverviews("BILINEAR", [2])
      ovr_data_ori = array.array(
          "B", ds.GetRasterBand(1).GetOverview(0).ReadRaster()
      )
      ds = None

      for path in (left_tif, right_tif):
        ds = gdal.Open(path, gdal.GA_Update)
        ds.GetRasterBand(1).Fill(0)
        ds = None

      alg = _get_refresh_alg()
      alg["dataset"] = tmp_vrt
      alg["resampling"] = "bilinear"
      alg["like"] = right_tif

      if with_progress:
        tab_pct = [0.0]

        def my_progress(pct: float, msg: str, user_data: object) -> bool:
          del msg, user_data
          if pct < tab_pct[0]:
            return False
          tab_pct[0] = pct
          return True

        self.assertTrue(alg.Run(my_progress))
        self.assertEqual(tab_pct[0], 1.0)
      else:
        self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      ds = gdal.Open(tmp_vrt)
      ovr_band = ds.GetRasterBand(1).GetOverview(0)
      ovr_data_refreshed = array.array("B", ovr_band.ReadRaster())
      for j in range(10):
        for i in range(5):
          idx = j * ovr_band.XSize + (i + 5)
          self.assertEqual(ovr_data_refreshed[idx], 0)
          ovr_data_refreshed[idx] = ovr_data_ori[idx]
      self.assertEqual(ovr_data_refreshed, ovr_data_ori)
      ds = None

  def test_like_errors(self) -> None:
    ds_no_gt = gdal.GetDriverByName("MEM").Create("", 2, 2)
    ds_no_gt.BuildOverviews("NEAR", [2])
    alg = _get_refresh_alg()
    alg["dataset"] = ds_no_gt
    alg["like"] = "/i_do/not/exist"
    with self.assertRaisesRegex(RuntimeError, "Dataset has no geotransform"):
      alg.Run()

    ds_bad_gt = gdal.GetDriverByName("MEM").Create("", 2, 2)
    ds_bad_gt.SetGeoTransform([0, 0, 0, 0, 0, 0])
    ds_bad_gt.BuildOverviews("NEAR", [2])
    alg2 = _get_refresh_alg()
    alg2["dataset"] = ds_bad_gt
    alg2["like"] = "/i_do/not/exist"
    with self.assertRaisesRegex(RuntimeError, "Cannot invert geotransform"):
      alg2.Run()

    ds_ok = gdal.GetDriverByName("MEM").Create("", 2, 2)
    ds_ok.SetGeoTransform([0, 1, 0, 0, 0, -1])
    ds_ok.BuildOverviews("NEAR", [2])
    alg3 = _get_refresh_alg()
    alg3["dataset"] = ds_ok
    alg3["like"] = "/i_do/not/exist"
    with self.assertRaisesRegex(RuntimeError, "/i_do/not/exist"):
      alg3.Run()

    with utilities_util.vsimem_dir("ovr_refresh_like_nogt") as tmp:
      src_path = f"{tmp}/source.tif"
      gdal.GetDriverByName("GTiff").Create(src_path, 1, 1)
      alg4 = _get_refresh_alg()
      alg4["dataset"] = ds_ok
      alg4["like"] = src_path
      with self.assertRaisesRegex(
          RuntimeError, "Source dataset has no geotransform"
      ):
        alg4.Run()

  def test_source_timestamp_vrt(self) -> None:
    if not _vrt_has_open_support():
      self.skipTest("VRT driver open missing")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      left_tif = os.path.join(tmpdir, "left.tif")
      right_tif = os.path.join(tmpdir, "right.tif")
      tmp_vrt = os.path.join(tmpdir, "tmp.vrt")

      gdal.Translate(left_tif, byte_tif, options="-srcwin 0 0 10 20")
      gdal.Translate(right_tif, byte_tif, options="-srcwin 10 0 10 20")
      ds = gdal.BuildVRT(tmp_vrt, [left_tif, right_tif])
      ds.BuildOverviews("BILINEAR", [2, 4])
      ovr_data_ori = array.array(
          "B", ds.GetRasterBand(1).GetOverview(0).ReadRaster()
      )
      ds = None

      ds = gdal.Open(left_tif, gdal.GA_Update)
      ds.GetRasterBand(1).Fill(0)
      ds = None

      ovr_mtime = int(os.stat(tmp_vrt + ".ovr").st_mtime)
      os.utime(left_tif, times=(ovr_mtime - 10, ovr_mtime - 10))

      ds = gdal.Open(right_tif, gdal.GA_Update)
      ds.GetRasterBand(1).Fill(0)
      ds = None
      os.utime(right_tif, times=(ovr_mtime + 10, ovr_mtime + 10))

      tab_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: object) -> bool:
        del msg, user_data
        if pct < tab_pct[0]:
          return False
        tab_pct[0] = pct
        return True

      alg = _get_refresh_alg()
      alg["dataset"] = tmp_vrt
      alg["resampling"] = "bilinear"
      alg["use-source-timestamp"] = True
      self.assertTrue(alg.Run(my_progress))
      self.assertTrue(alg.Finalize())
      self.assertEqual(tab_pct[0], 1.0)

      ds = gdal.Open(tmp_vrt)
      ovr_band = ds.GetRasterBand(1).GetOverview(0)
      ovr_data_refreshed = array.array("B", ovr_band.ReadRaster())
      for j in range(10):
        for i in range(5):
          idx = j * ovr_band.XSize + (i + 5)
          self.assertEqual(ovr_data_refreshed[idx], 0)
          ovr_data_refreshed[idx] = ovr_data_ori[idx]
      self.assertEqual(ovr_data_refreshed, ovr_data_ori)
      ds = None

  def test_source_timestamp_gti(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    gti_drv = gdal.GetDriverByName("GTI")
    if gti_drv is None:
      self.skipTest("GTI driver not available")
    if gti_drv.GetMetadataItem("IS_PLUGIN"):
      self.skipTest("GTI driver is a plugin")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      left_tif = os.path.join(tmpdir, "left.tif")
      right_tif = os.path.join(tmpdir, "right.tif")
      tmp_gti = os.path.join(tmpdir, "test.gti.gpkg")

      gdal.Translate(left_tif, byte_tif, options="-srcwin 0 0 10 20")
      gdal.Translate(right_tif, byte_tif, options="-srcwin 10 0 10 20")

      source_ds = [gdal.Open(left_tif), gdal.Open(right_tif)]
      index_ds = ogr.GetDriverByName("GPKG").CreateDataSource(tmp_gti)
      lyr = index_ds.CreateLayer("index", srs=source_ds[0].GetSpatialRef())
      lyr.CreateField(ogr.FieldDefn("location"))
      for src_ds in source_ds:
        feat = ogr.Feature(lyr.GetLayerDefn())
        src_gt = src_ds.GetGeoTransform()
        minx = src_gt[0]
        maxx = minx + src_ds.RasterXSize * src_gt[1]
        maxy = src_gt[3]
        miny = maxy + src_ds.RasterYSize * src_gt[5]
        feat["location"] = src_ds.GetDescription()
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt(
                f"POLYGON(({minx} {miny},{minx} {maxy},"
                f"{maxx} {maxy},{maxx} {miny},{minx} {miny}))"
            )
        )
        lyr.CreateFeature(feat)
      index_ds.Close()
      del source_ds

      ds = gdal.Open(tmp_gti, gdal.GA_Update)
      ds.BuildOverviews("BILINEAR", [2, 4])
      ovr_data_ori = array.array(
          "B", ds.GetRasterBand(1).GetOverview(0).ReadRaster()
      )
      ds = None

      ds = gdal.Open(left_tif, gdal.GA_Update)
      ds.GetRasterBand(1).Fill(0)
      ds = None
      ovr_mtime = int(os.stat(tmp_gti + ".ovr").st_mtime)
      os.utime(left_tif, times=(ovr_mtime - 10, ovr_mtime - 10))

      ds = gdal.Open(right_tif, gdal.GA_Update)
      ds.GetRasterBand(1).Fill(0)
      ds = None
      os.utime(right_tif, times=(ovr_mtime + 10, ovr_mtime + 10))

      alg = _get_refresh_alg()
      alg["dataset"] = tmp_gti
      alg["resampling"] = "bilinear"
      alg["use-source-timestamp"] = True
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      ds = gdal.Open(tmp_gti)
      ovr_band = ds.GetRasterBand(1).GetOverview(0)
      ovr_data_refreshed = array.array("B", ovr_band.ReadRaster())
      for j in range(10):
        for i in range(5):
          idx = j * ovr_band.XSize + (i + 5)
          self.assertEqual(ovr_data_refreshed[idx], 0)
          ovr_data_refreshed[idx] = ovr_data_ori[idx]
      self.assertEqual(ovr_data_refreshed, ovr_data_ori)
      ds = None

  def test_errors_and_timestamp_validation(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    ds_mem = gdal.Translate(
        "", byte_tif, options="-f MEM -outsize 512 512 -r cubic"
    )
    ds_mem.BuildOverviews("BILINEAR", [2])
    alg = _get_refresh_alg()
    alg["dataset"] = ds_mem
    alg["use-source-timestamp"] = True
    with self.assertRaisesRegex(RuntimeError, "Cannot find .ovr"):
      alg.Run()

    with utilities_util.vsimem_dir("ovr_refresh_ts_err") as tmp:
      out_tif = f"{tmp}/out.tif"
      gdal.Translate(
          out_tif, byte_tif, options="-f GTiff -outsize 512 512 -r cubic"
      )
      ds_tif = gdal.Open(out_tif)
      ds_tif.BuildOverviews("BILINEAR", [2])

      alg_mtime = _get_refresh_alg()
      alg_mtime["dataset"] = ds_tif
      alg_mtime["use-source-timestamp"] = True
      with gdal.config_option("CPL_VSI_MEM_MTIME", "0"):
        with self.assertRaisesRegex(
            RuntimeError, "Cannot get modification time"
        ):
          alg_mtime.Run()

      alg_non_vrt = _get_refresh_alg()
      alg_non_vrt["dataset"] = ds_tif
      alg_non_vrt["use-source-timestamp"] = True
      with self.assertRaisesRegex(
          RuntimeError,
          "--use-source-timestamp only works on a VRT or GTI dataset",
      ):
        alg_non_vrt.Run()
      ds_tif = None

    alg_no_band = _get_refresh_alg()
    alg_no_band["dataset"] = gdal.GetDriverByName("MEM").Create("", 1, 1, 0)
    with self.assertRaisesRegex(RuntimeError, "Dataset has no raster band"):
      alg_no_band.Run()

    alg_no_ovr = _get_refresh_alg()
    alg_no_ovr["dataset"] = gdal.GetDriverByName("MEM").Create("", 1, 1)
    with self.assertRaisesRegex(RuntimeError, "No overviews to refresh"):
      alg_no_ovr.Run()

    ds_lvl = gdal.GetDriverByName("MEM").Create("", 4, 4)
    ds_lvl.BuildOverviews("NEAR", [4])
    alg_lvl = _get_refresh_alg()
    alg_lvl["dataset"] = ds_lvl
    alg_lvl["levels"] = 2
    with self.assertRaisesRegex(
        RuntimeError, "Cannot find overview level with subsampling factor of 2"
    ):
      alg_lvl.Run()


if __name__ == "__main__":
  absltest.main()
