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

"""Tests for `gdal dataset delete`.

Ported from upstream autotest/utilities/test_gdalalg_dataset_delete.py.

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


class GdalalgDatasetDeleteTest(gdaltest.TestCase):

  def test_delete(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("dataset_delete") as tmp:
      in_tif = f"{tmp}/in.tif"
      gdal.Translate(in_tif, byte_tif)
      self.assertTrue(gdal.Run("dataset", "delete", filename=in_tif))
      self.assertIsNone(gdal.VSIStatL(in_tif))

  def test_delete_with_format(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("dataset_delete_fmt") as tmp:
      in_tif = f"{tmp}/in.tif"
      gdal.Translate(in_tif, byte_tif)
      self.assertTrue(
          gdal.Run("dataset", "delete", format="GTiff", filename=in_tif)
      )
      self.assertIsNone(gdal.VSIStatL(in_tif))

  def test_delete_error(self) -> None:
    with self.assertRaisesRegex(
        RuntimeError, "No identifiable driver for /i_do/not/exist.tif"
    ):
      gdal.Run("dataset", "delete", filename="/i_do/not/exist.tif")

  def test_completion(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "dataset", "delete", "--format="]
    )
    self.assertEqual(code, 0)
    self.assertIn("GTiff ", out)
    self.assertIn("ESRI\\ Shapefile ", out)


if __name__ == "__main__":
  absltest.main()
