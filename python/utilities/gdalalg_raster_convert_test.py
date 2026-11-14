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

"""Tests for `gdal raster convert`.

Ported from upstream autotest/utilities/test_gdalalg_raster_convert.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_convert_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  raster = reg.InstantiateAlg("raster")
  return raster.InstantiateSubAlgorithm("convert")


class GdalalgRasterConvertTest(gdaltest.TestCase):

  def test_convert_overwrite_and_co(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    with utilities_util.vsimem_dir("raster_convert") as tmp:
      out_filename = f"{tmp}/out.tif"

      convert = _get_convert_alg()
      self.assertTrue(convert.ParseRunAndFinalize([utmsmall, out_filename]))

      with gdal.Open(out_filename, gdal.GA_Update) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)
        ds.GetRasterBand(1).Fill(0)

      convert = _get_convert_alg()
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        convert.ParseRunAndFinalize([utmsmall, out_filename])

      convert = _get_convert_alg()
      self.assertTrue(
          convert.ParseRunAndFinalize(
              ["--overwrite", "--co=TILED=YES", utmsmall, out_filename]
          )
      )

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [256, 256])
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 50054)

  def test_convert_to_mem(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    convert = _get_convert_alg()
    self.assertTrue(
        convert.ParseCommandLineArguments(["--of=MEM", utmsmall, ""])
    )
    self.assertTrue(convert.Run())
    out_ds = utilities_util.get_arg_dataset(convert, "output")
    self.assertIsNotNone(out_ds)
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 50054)

  def test_convert_append_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    with utilities_util.vsimem_dir("raster_convert_gpkg") as tmp:
      out_filename = f"{tmp}/out.gpkg"

      convert = _get_convert_alg()
      self.assertTrue(convert.ParseRunAndFinalize([utmsmall, out_filename]))

      convert = _get_convert_alg()
      self.assertTrue(
          convert.ParseRunAndFinalize([
              utmsmall,
              out_filename,
              "--append",
              "--co",
              "RASTER_TABLE=new_table",
          ])
      )

      with gdal.Open(out_filename) as ds:
        self.assertLen(ds.GetSubDatasets(), 2)

  def test_convert_failed_append_hfa(self) -> None:
    if not utilities_util.has_driver("HFA"):
      self.skipTest("HFA driver not available")
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    with utilities_util.vsimem_dir("raster_convert_hfa") as tmp:
      out_filename = f"{tmp}/test.img"

      convert = _get_convert_alg()
      self.assertTrue(convert.ParseRunAndFinalize([utmsmall, out_filename]))

      convert = _get_convert_alg()
      with self.assertRaisesRegex(
          RuntimeError, "Subdataset creation not supported for driver HFA"
      ):
        convert.ParseRunAndFinalize([utmsmall, out_filename, "--append"])

  def test_convert_comma_in_filename(self) -> None:
    with utilities_util.vsimem_dir("raster_convert_comma") as tmp:
      src_filename = f"{tmp}/a,b.tif"
      out_filename = f"{tmp}/out.tif"
      gdal.GetDriverByName("GTiff").Create(src_filename, 1, 1)

      convert = _get_convert_alg()
      self.assertTrue(convert.ParseRunAndFinalize([src_filename, out_filename]))


if __name__ == "__main__":
  absltest.main()
