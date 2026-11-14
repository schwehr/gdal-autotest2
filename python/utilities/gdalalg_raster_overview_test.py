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

"""Tests for `gdal raster overview`.

Ported from upstream autotest/utilities/test_gdalalg_raster_overview.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_overview_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["overview"]


def _get_overview_add_alg() -> gdal.Algorithm:
  return _get_overview_alg()["add"]


def _get_overview_delete_alg() -> gdal.Algorithm:
  return _get_overview_alg()["delete"]


@contextlib.contextmanager
def _capture_errors() -> Iterator[list[int]]:
  err_types: list[int] = []

  def _handler(err_class: int, unused_err_no: int, unused_msg: str) -> None:
    err_types.append(err_class)

  gdal.ErrorReset()
  gdal.PushErrorHandler(_handler)
  try:
    yield err_types
  finally:
    gdal.PopErrorHandler()


class GdalalgRasterOverviewTest(gdaltest.TestCase, parameterized.TestCase):

  def test_invalid_arguments(self) -> None:
    add = _get_overview_add_alg()
    with self.assertRaises(RuntimeError):
      add["levels"] = [1]

    add = _get_overview_add_alg()
    with self.assertRaises(RuntimeError):
      add["min-size"] = 0

  def test_explicit_level(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    ds = gdal.Translate("", byte_tif, format="MEM")

    add = _get_overview_add_alg()
    add["dataset"] = ds
    add["levels"] = [2]
    self.assertTrue(add.Run())

    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
    self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1087)

  def test_minsize_and_resampling(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    ds = gdal.Translate("", byte_tif, format="MEM")

    add = _get_overview_add_alg()
    add["dataset"] = ds
    add["resampling"] = "average"
    add["min-size"] = 10
    self.assertTrue(add.Run())

    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
    self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
    self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1152)

  def test_reuse_resampling_and_levels(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("overview_reuse") as tmpdir:
      tmp_filename = f"{tmpdir}/tmp.tif"
      ds = gdal.Translate(tmp_filename, byte_tif)

      add = _get_overview_add_alg()
      add["dataset"] = ds
      add["resampling"] = "average"
      add["min-size"] = 10
      self.assertTrue(add.Run())

      band = ds.GetRasterBand(1)
      self.assertEqual(band.Checksum(), 4672)
      self.assertEqual(band.GetOverviewCount(), 1)
      self.assertEqual(
          band.GetOverview(0).GetMetadataItem("RESAMPLING"), "AVERAGE"
      )
      self.assertEqual(band.GetOverview(0).Checksum(), 1152)

      band.GetOverview(0).Fill(0)

      add = _get_overview_add_alg()
      add["dataset"] = ds
      self.assertTrue(add.Run())

      self.assertEqual(band.Checksum(), 4672)
      self.assertEqual(band.GetOverviewCount(), 1)
      self.assertEqual(
          band.GetOverview(0).GetMetadataItem("RESAMPLING"), "AVERAGE"
      )
      self.assertEqual(band.GetOverview(0).Checksum(), 1152)
      ds = None

  def test_in_place(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("overview_in_place") as tmpdir:
      tmp_filename = f"{tmpdir}/tmp.tif"
      gdal.Translate(tmp_filename, byte_tif)

      add = _get_overview_add_alg()
      self.assertTrue(
          add.ParseRunAndFinalize(
              [tmp_filename, "--levels=2", "--co", "BLOCKSIZE=64"]
          )
      )

      self.assertIsNone(gdal.VSIStatL(f"{tmp_filename}.ovr"))

      with gdal.Open(tmp_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
        self.assertEqual(
            ds.GetRasterBand(1).GetOverview(0).GetBlockSize(), [64, 64]
        )

  def test_external_add_and_delete(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("overview_external") as tmpdir:
      tmp_filename = f"{tmpdir}/tmp.tif"
      gdal.Translate(tmp_filename, byte_tif)

      add = _get_overview_add_alg()
      self.assertTrue(
          add.ParseRunAndFinalize([tmp_filename, "--levels=2", "--external"])
      )

      self.assertIsNotNone(gdal.VSIStatL(f"{tmp_filename}.ovr"))

      with gdal.Open(tmp_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)

      delete = _get_overview_delete_alg()
      self.assertTrue(delete.ParseRunAndFinalize([tmp_filename, "--external"]))

      self.assertIsNone(gdal.VSIStatL(f"{tmp_filename}.ovr"))

  def test_external_incompatible_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("overview_gpkg") as tmpdir:
      tmp_filename = f"{tmpdir}/tmp.gpkg"
      gdal.Translate(tmp_filename, byte_tif)

      add = _get_overview_add_alg()
      with self.assertRaisesRegex(
          RuntimeError, "Driver GPKG does not support external overviews"
      ):
        add.ParseRunAndFinalize([tmp_filename, "--levels=2", "--external"])

  def test_external_other_format_png(self) -> None:
    if not utilities_util.has_driver("PNG"):
      self.skipTest("PNG driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("overview_png") as tmpdir:
      tmp_filename = f"{tmpdir}/tmp.png"
      gdal.Translate(tmp_filename, byte_tif)

      add = _get_overview_add_alg()
      self.assertTrue(
          add.ParseRunAndFinalize([tmp_filename, "--external", "--levels=2"])
      )

      self.assertIsNotNone(gdal.VSIStatL(f"{tmp_filename}.ovr"))

      with gdal.Open(tmp_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)

  def test_external_rrd(self) -> None:
    if not utilities_util.has_driver("HFA"):
      self.skipTest("HFA driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("overview_rrd") as tmpdir:
      tmp_filename = f"{tmpdir}/tmp.tif"
      gdal.Translate(tmp_filename, byte_tif)

      try:
        gdal.Run(
            _get_overview_add_alg(),
            input=tmp_filename,
            levels=[2],
            creation_option={"LOCATION": "RRD"},
        )
      except RuntimeError:
        err_msg = gdal.GetLastErrorMsg()
        if "This build does not support creating .aux overviews" in err_msg:
          self.skipTest(err_msg)
        raise

      self.assertIsNotNone(gdal.VSIStatL(f"{tmpdir}/tmp.aux"))

      with gdal.Open(tmp_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)

  def test_delete(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    ds = gdal.Translate("", byte_tif, format="MEM")

    add = _get_overview_add_alg()
    add["dataset"] = ds
    add["resampling"] = "average"
    add["min-size"] = 10
    self.assertTrue(add.Run())
    self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)

    delete = _get_overview_delete_alg()
    delete["dataset"] = ds
    self.assertTrue(delete.Run())
    self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)

  def test_cog(self) -> None:
    if not utilities_util.has_driver("COG"):
      self.skipTest("COG driver not available")

    with utilities_util.vsimem_dir("overview_cog") as tmpdir:
      src_ds = gdal.GetDriverByName("MEM").Create("", 1024, 1024)
      filename = f"{tmpdir}/my_cog.tif"
      gdal.GetDriverByName("COG").CreateCopy(filename, src_ds)

      with gdal.Open(filename) as ds:
        self.assertGreater(ds.GetRasterBand(1).GetOverviewCount(), 0)

      delete = _get_overview_delete_alg()
      delete["dataset"] = filename
      self.assertTrue(delete.Run())
      self.assertTrue(delete.Finalize())

      with gdal.Open(filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)

      add = _get_overview_add_alg()
      add["dataset"] = filename
      with self.assertRaisesRegex(
          RuntimeError, r"has C\(loud\) O\(ptimized\) G\(eoTIFF\) layout"
      ):
        add.Run()

      add = _get_overview_add_alg()
      add["dataset"] = filename
      add["open-option"] = {"IGNORE_COG_LAYOUT_BREAK": "YES"}
      with _capture_errors() as errs:
        self.assertTrue(add.Run())
      self.assertIn(gdal.CE_Warning, errs)
      self.assertTrue(add.Finalize())

      with gdal.Open(filename) as ds:
        self.assertGreater(ds.GetRasterBand(1).GetOverviewCount(), 0)

  @parameterized.parameters(
      (True, True),
      (True, False),
      (False, True),
      (False, False),
  )
  def test_add_from_dataset(self, external: bool, with_progress: bool) -> None:
    with utilities_util.vsimem_dir(
        f"overview_add_ds_{external}_{with_progress}"
    ) as tmpdir:
      out_path = f"{tmpdir}/out.tif"
      in_path = f"{tmpdir}/in.tif"

      out_ds = gdal.GetDriverByName("GTiff").Create(out_path, 10, 10)
      out_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
      out_ds.SetSpatialRef(osr.SpatialReference(epsg=4326))
      out_ds = None

      ds = gdal.GetDriverByName("GTiff").Create(in_path, 2, 5)
      ds.SetGeoTransform([2, 5, 0, 49, 0, -2])
      ds.SetSpatialRef(osr.SpatialReference(epsg=4326))
      ds.GetRasterBand(1).Fill(1)
      ds = None

      last_pct = [0.0]

      def my_progress(
          pct: float, unused_msg: str, unused_user_data: object
      ) -> bool:
        last_pct[0] = pct
        return True

      progress = my_progress if with_progress else None

      gdal.Run(
          "raster",
          "overview",
          "add",
          external=external,
          overview_src=in_path,
          dataset=out_path,
          progress=progress,
      )

      if with_progress:
        self.assertEqual(last_pct[0], 1.0)

      self.assertEqual(gdal.VSIStatL(f"{out_path}.ovr") is not None, external)

      ds = gdal.Open(out_path, gdal.GA_Update)
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
      ovr = ds.GetRasterBand(1).GetOverview(0)
      self.assertEqual(ovr.XSize, 2)
      self.assertEqual(ovr.YSize, 5)
      self.assertEqual(ovr.ComputeRasterMinMax(False), (1, 1))
      ovr.Fill(0)
      ds = None

      last_pct[0] = 0.0
      gdal.Run(
          "raster",
          "overview",
          "add",
          external=external,
          overview_src=in_path,
          dataset=out_path,
          progress=progress,
      )

      if with_progress:
        self.assertEqual(last_pct[0], 1.0)

      self.assertEqual(gdal.VSIStatL(f"{out_path}.ovr") is not None, external)

      with gdal.Open(out_path) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
        ovr = ds.GetRasterBand(1).GetOverview(0)
        self.assertEqual(ovr.XSize, 2)
        self.assertEqual(ovr.YSize, 5)
        self.assertEqual(ovr.ComputeRasterMinMax(False), (1, 1))

  def test_add_from_dataset_errors(self) -> None:
    mem_out = gdal.GetDriverByName("MEM").Create("", 10, 10)
    mem_ovr = gdal.GetDriverByName("MEM").Create("", 6, 5)
    with self.assertRaisesRegex(
        RuntimeError, r"AddOverviews\(\) not supported for this dataset"
    ):
      gdal.Run(
          "raster", "overview", "add", overview_src=mem_ovr, dataset=mem_out
      )

    with utilities_util.vsimem_dir("overview_add_errors") as tmpdir:
      out_ds = gdal.GetDriverByName("GTIFF").Create(
          f"{tmpdir}/out1.tif", 10, 10
      )
      ovr_ds = gdal.GetDriverByName("MEM").Create("", 11, 5)
      with self.assertRaisesRegex(
          RuntimeError,
          r"AddOverviews\(\): at least one input dataset has dimensions"
          r" larger than the full resolution dataset",
      ):
        gdal.Run(
            "raster", "overview", "add", overview_src=ovr_ds, dataset=out_ds
        )

      ovr_ds = gdal.GetDriverByName("MEM").Create("", 0, 0)
      with self.assertRaisesRegex(
          RuntimeError,
          r"AddOverviews\(\): at least one input dataset has one of its"
          r" dimensions equal to 0",
      ):
        gdal.Run(
            "raster", "overview", "add", overview_src=ovr_ds, dataset=out_ds
        )

      ovr_ds = gdal.GetDriverByName("MEM").Create("", 5, 5, 2)
      with self.assertRaisesRegex(
          RuntimeError,
          r"AddOverviews\(\): at least one input dataset not the same number"
          r" of bands than the full resolution dataset",
      ):
        gdal.Run(
            "raster", "overview", "add", overview_src=ovr_ds, dataset=out_ds
        )

      out_ds.SetSpatialRef(osr.SpatialReference(epsg=4326))
      ovr_ds = gdal.GetDriverByName("MEM").Create("", 5, 5)
      ovr_ds.SetSpatialRef(osr.SpatialReference(epsg=32631))
      with self.assertRaisesRegex(
          RuntimeError,
          r"AddOverviews\(\): at least one input dataset has its CRS different"
          r" from the one of the full resolution dataset",
      ):
        gdal.Run(
            "raster", "overview", "add", overview_src=ovr_ds, dataset=out_ds
        )

      out_ds2 = gdal.GetDriverByName("GTIFF").Create(
          f"{tmpdir}/out2.tif", 10, 10
      )
      out_ds2.SetGeoTransform([2, 1, 0, 49, 0, -1])
      ovr_ds = gdal.GetDriverByName("MEM").Create("", 5, 5)
      ovr_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
      with self.assertRaisesRegex(
          RuntimeError,
          r"AddOverviews\(\): at least one input dataset has its geospatial"
          r" extent different from the one of the full resolution dataset",
      ):
        gdal.Run(
            "raster", "overview", "add", overview_src=ovr_ds, dataset=out_ds2
        )

  def test_add_complete(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "overview", "add", byte_tif, "--co"]
    )
    self.assertEqual(code, 0)
    tokens = out.split(" ")
    self.assertIn("LOCATION=", tokens)
    self.assertIn("COMPRESS=", tokens)

    out, _, code = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "raster",
        "overview",
        "add",
        byte_tif,
        "--co",
        "COMPRESS=",
    ])
    self.assertEqual(code, 0)
    tokens = out.split(" ")
    self.assertIn("NONE", tokens)
    self.assertIn("LZW", tokens)

  def test_add_in_pipeline(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    src_ds = gdal.Translate("", byte_tif, format="MEM")

    with gdal.Run(
        "pipeline",
        input=src_ds,
        pipeline=(
            "read ! overview add --levels 2 ! write --format stream"
            " streamed_dataset"
        ),
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)
      self.assertEqual(ds.GetRasterBand(1).GetOverview(0).Checksum(), 1192)

    self.assertEqual(src_ds.GetRasterBand(1).GetOverviewCount(), 0)


if __name__ == "__main__":
  absltest.main()
