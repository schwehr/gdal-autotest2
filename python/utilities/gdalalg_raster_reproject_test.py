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

"""Tests for `gdal raster reproject`.

Ported from upstream autotest/utilities/test_gdalalg_raster_reproject.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os

from absl.testing import absltest
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_reproject_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["reproject"]


def _has_proj_8_1() -> bool:
  return (osr.GetPROJVersionMajor(), osr.GetPROJVersionMinor()) >= (8, 1)


class GdalalgRasterReprojectTest(gdaltest.TestCase):

  def test_reproject_basic(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_reproject_basic") as tmp:
      out_filename = f"{tmp}/out.tif"
      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: object) -> bool:
        del msg, user_data
        if pct < last_pct[0]:
          return False
        last_pct[0] = pct
        return True

      alg = _get_reproject_alg()
      alg["src-crs"] = "EPSG:32611"
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      alg["dst-crs"] = srs
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["creation-option"] = {"COMPRESS": "LZW"}
      self.assertTrue(alg.Run(my_progress))
      self.assertTrue(alg.Finalize())
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4727)
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )

  def test_reproject_through_pipeline(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_reproject_pipeline") as tmp:
      for suffix, extra_step in (
          ("opt", ""),
          ("non_opt", " ! edit"),
      ):
        out_filename = f"{tmp}/out_{suffix}.tif"
        last_pct = [0.0]

        def my_progress(pct: float, msg: str, user_data: object) -> bool:
          del msg, user_data
          if pct < last_pct[0]:
            return False
          last_pct[0] = pct
          return True

        with gdal.Run(
            "raster",
            "pipeline",
            pipeline=(
                f"read {byte_tif} ! reproject --src-crs=EPSG:32611"
                f" --dst-crs=EPSG:4326{extra_step} ! write {out_filename}"
                " --co COMPRESS=LZW"
            ),
            progress=my_progress,
        ) as alg:
          self.assertIsNotNone(alg)
        self.assertEqual(last_pct[0], 1.0)

        with gdal.OpenEx(out_filename) as ds:
          self.assertEqual(ds.GetRasterBand(1).Checksum(), 4727)
          self.assertEqual(
              ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
          )

  def test_reproject_failure(self) -> None:
    nan32_tif = utilities_util.get_testdata_path("../gcore/data/nan32.tif")
    with utilities_util.vsimem_dir("raster_reproject_failure") as tmp:
      out_filename = f"{tmp}/out.tif"
      alg = _get_reproject_alg()
      with self.assertRaisesRegex(
          RuntimeError, "Unable to compute a transformation"
      ):
        alg.ParseRunAndFinalize([
            "--src-crs=EPSG:32611",
            "--dst-crs=EPSG:4326",
            nan32_tif,
            out_filename,
        ])

  def test_reproject_size_and_bbox_crs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_reproject_geom") as tmp:
      out_size = f"{tmp}/out_size.tif"
      alg = _get_reproject_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(["--size=10,0", byte_tif, out_size])
      )
      with gdal.OpenEx(out_size) as ds:
        self.assertEqual(ds.RasterXSize, 10)
        self.assertEqual(ds.RasterYSize, 10)

      out_bbox = f"{tmp}/out_bbox.tif"
      alg2 = _get_reproject_alg()
      self.assertTrue(
          alg2.ParseRunAndFinalize([
              (
                  "--bbox=-117.638051657173,33.8904636339659,"
                  "-117.627303823822,33.8995379597727"
              ),
              "--bbox-crs=EPSG:4267",
              byte_tif,
              out_bbox,
          ])
      )
      with gdal.OpenEx(out_bbox) as ds:
        self.assertEqual(ds.RasterXSize, 17)
        self.assertEqual(ds.RasterYSize, 17)

  def test_nodata_add_alpha_and_warp_options(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
    src_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])
    src_ds.WriteRaster(0, 0, 2, 1, b"\x00\x01")

    alg = _get_reproject_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-nodata"] = [1]
    alg["dst-nodata"] = [2]
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).GetNoDataValue(), 2)
    self.assertEqual(out_ds.GetRasterBand(1).ReadRaster(), b"\x00\x02")

    src_alpha = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_alpha.SetGeoTransform([0, 1, 0, 0, 0, -1])
    alg_alpha = _get_reproject_alg()
    alg_alpha["input"] = src_alpha
    alg_alpha["output"] = ""
    alg_alpha["output-format"] = "MEM"
    alg_alpha["add-alpha"] = True
    self.assertTrue(alg_alpha.Run())
    out_alpha = utilities_util.get_arg_dataset(alg_alpha, "output")
    self.assertEqual(out_alpha.RasterCount, 2)
    self.assertEqual(
        out_alpha.GetRasterBand(2).GetColorInterpretation(), gdal.GCI_AlphaBand
    )

    src_wo = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    src_wo.SetGeoTransform([0, 1, 0, 0, 0, -1])
    src_wo.GetRasterBand(1).SetNoDataValue(0)
    src_wo.GetRasterBand(2).SetNoDataValue(1)
    src_wo.GetRasterBand(2).WriteRaster(0, 0, 1, 1, b"\x02")
    alg_wo = _get_reproject_alg()
    alg_wo["input"] = src_wo
    alg_wo["output"] = ""
    alg_wo["output-format"] = "MEM"
    alg_wo["warp-option"] = ["UNIFIED_SRC_NODATA=YES"]
    alg_wo["dst-nodata"] = [3, 4]
    self.assertTrue(alg_wo.Run())
    out_wo = utilities_util.get_arg_dataset(alg_wo, "output")
    self.assertEqual(out_wo.GetRasterBand(1).ReadRaster(), b"\x00")

  def test_transform_option_error_threshold_and_threads(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.SetGeoTransform([0, 1, 0, 0, 0, -1])

    alg = _get_reproject_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["transform-option"] = ["METHOD=RPC"]
    with self.assertRaisesRegex(
        RuntimeError, "Unable to compute a RPC based transformation"
    ):
      alg.Run()

    alg_err = _get_reproject_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Value of argument 'error-threshold' is -1, but should be >= 0",
    ):
      alg_err["error-threshold"] = -1

    with utilities_util.vsimem_dir("raster_reproject_threads") as tmp:
      out_filename = f"{tmp}/out.tif"
      alg_threads = _get_reproject_alg()
      self.assertTrue(
          alg_threads.ParseRunAndFinalize([
              "--src-crs=EPSG:32611",
              "--dst-crs=EPSG:4326",
              byte_tif,
              "--wo=NUM_THREADS=2",
              out_filename,
          ])
      )

      alg_conflict = _get_reproject_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "--num-threads argument and NUM_THREADS warp options are mutually"
          " exclusive",
      ):
        alg_conflict.ParseRunAndFinalize([
            "--src-crs=EPSG:32611",
            "--dst-crs=EPSG:4326",
            byte_tif,
            "--wo=NUM_THREADS=1",
            "--num-threads=2",
            f"{tmp}/out_conflict.tif",
        ])

  def test_complete_dst_crs_epsg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "raster",
        "reproject",
        byte_tif,
        "--dst-crs=EPSG:",
    ])
    if code == -6:
      self.skipTest(
          "Upstream GDALInConstructionAlgorithmArg::SetIsCRSArg std::sort "
          "comparator is not strict-weak-ordering under libc++ hardening; "
          "see gdalalgorithm.cpp:1618"
      )
    self.assertEqual(code, 0)
    self.assertIn("4326\\ --", out)
    self.assertNotIn("2193\\ --", out)

  def test_complete_dst_crs_iau(self) -> None:
    if not _has_proj_8_1():
      self.skipTest("Requires PROJ >= 8.1")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "raster",
        "reproject",
        byte_tif,
        "--dst-crs=IAU:",
    ])
    if code == -6:
      self.skipTest(
          "Upstream GDALInConstructionAlgorithmArg::SetIsCRSArg std::sort "
          "comparator is not strict-weak-ordering under libc++ hardening; "
          "see gdalalgorithm.cpp:1618"
      )
    self.assertEqual(code, 0)
    self.assertIn("Earth", out)
    self.assertNotIn("Mars", out)

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      in_tif = os.path.join(tmpdir, "in.tif")
      ds = gdal.GetDriverByName("GTiff").Create(in_tif, 1, 1)
      srs = osr.SpatialReference()
      srs.SetFromUserInput("IAU:49900")
      ds.SetSpatialRef(srs)
      ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
      ds = None

      out_mars, _, code = utilities_util.run_gdal_cli([
          "completion",
          "gdal",
          "raster",
          "reproject",
          in_tif,
          "--dst-crs=IAU:",
      ])
      self.assertEqual(code, 0)
      self.assertNotIn("Earth", out_mars)
      self.assertIn("Mars", out_mars)


if __name__ == "__main__":
  absltest.main()
