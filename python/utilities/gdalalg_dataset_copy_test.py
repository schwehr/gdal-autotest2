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

"""Tests for `gdal dataset copy`.

Ported from upstream autotest/utilities/test_gdalalg_dataset_copy.py.

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


class GdalalgDatasetCopyTest(gdaltest.TestCase):

  def test_copy_and_overwrite(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("dataset_copy_test") as tmp:
      out_tif = f"{tmp}/out.tif"
      self.assertTrue(
          gdal.Run("dataset", "copy", source=byte_tif, destination=out_tif)
      )

      with gdal.Open(out_tif) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), "GTiff")

      with self.assertRaisesRegex(
          RuntimeError,
          "already exists. Specify the --overwrite option to overwrite it",
      ):
        gdal.Run("dataset", "copy", source=byte_tif, destination=out_tif)

      gdal.FileFromMemBuffer(out_tif, "")

      self.assertTrue(
          gdal.Run(
              "dataset",
              "copy",
              format="GTiff",
              source=byte_tif,
              destination=out_tif,
              overwrite=True,
          )
      )

      with gdal.Open(out_tif) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), "GTiff")

  def test_overwrite_existing_directory(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("dataset_copy_dir_test") as tmp:
      my_dir = f"{tmp}/my_dir"
      gdal.Mkdir(my_dir, 0o755)

      with self.assertRaisesRegex(
          RuntimeError,
          "already exists, but is not recognized as a valid GDAL dataset."
          " Please manually delete it before retrying",
      ):
        gdal.Run(
            "dataset",
            "copy",
            source=byte_tif,
            destination=my_dir,
            overwrite=True,
        )

  def test_overwrite_existing_dataset_directory(self) -> None:
    drv = gdal.GetDriverByName("OpenFileGDB")
    if drv is None:
      self.skipTest("OpenFileGDB driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("dataset_copy_gdb_test") as tmp:
      out_gdb = f"{tmp}/out.gdb"
      drv.CreateVector(out_gdb)

      gdal.Run(
          "dataset",
          "copy",
          source=byte_tif,
          destination=out_gdb,
          overwrite=True,
      )

      with gdal.Open(out_gdb) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), "GTiff")

  def test_error(self) -> None:
    with utilities_util.vsimem_dir("dataset_copy_err_test") as tmp:
      with self.assertRaisesRegex(
          RuntimeError,
          "No identifiable driver for /i_do/not/exist.tif",
      ):
        gdal.Run(
            "dataset",
            "copy",
            source="/i_do/not/exist.tif",
            destination=f"{tmp}/out.tif",
        )

  def test_complete(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "dataset", "copy", "--format="]
    )
    self.assertEqual(code, 0)
    self.assertIn("GTiff ", out)
    self.assertIn("ESRI\\ Shapefile ", out)


if __name__ == "__main__":
  absltest.main()
