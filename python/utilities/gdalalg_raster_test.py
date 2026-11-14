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

"""Tests for `gdal raster`.

Ported from upstream autotest/utilities/test_gdalalg_raster.py.

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


def _get_raster_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  return reg.InstantiateAlg("raster")


class GdalalgRasterTest(gdaltest.TestCase):

  def test_run_error(self) -> None:
    raster_alg = _get_raster_alg()
    with self.assertRaisesRegex(
        RuntimeError, "method should not be called directly"
    ):
      raster_alg.Run()

  def test_drivers(self) -> None:
    with gdal.Run(["raster"], drivers=True) as alg:
      drivers = alg.Output()
      names = [x["short_name"] for x in drivers]
      self.assertIn("GTiff", names)
      self.assertNotIn("ESRI Shapefile", names)


if __name__ == "__main__":
  absltest.main()
