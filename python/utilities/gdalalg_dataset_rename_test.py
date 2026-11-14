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

"""Tests for `gdal dataset rename`.

Ported from upstream autotest/utilities/test_gdalalg_dataset_rename.py.

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


class GdalalgDatasetRenameTest(gdaltest.TestCase):

  def test_rename(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("dataset_rename") as tmp:
      src_path = f"{tmp}/in.tif"
      dst_path = f"{tmp}/out.tif"
      with gdal.Translate(src_path, byte_tif):
        pass

      self.assertTrue(
          gdal.Run("dataset", "rename", source=src_path, destination=dst_path)
      )

      with gdal.Open(dst_path) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), "GTiff")

      self.assertIsNone(gdal.VSIStatL(src_path))

  def test_rename_error(self) -> None:
    with utilities_util.vsimem_dir("dataset_rename_err") as tmp:
      with self.assertRaisesRegex(
          RuntimeError, "No identifiable driver for /i_do/not/exist.tif"
      ):
        gdal.Run(
            "dataset",
            "rename",
            source="/i_do/not/exist.tif",
            destination=f"{tmp}/out.tif",
        )


if __name__ == "__main__":
  absltest.main()
