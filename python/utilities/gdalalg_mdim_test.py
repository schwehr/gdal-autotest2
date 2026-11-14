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

"""Tests for `gdal mdim`.

Ported from upstream autotest/utilities/test_gdalalg_mdim.py.

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


class GdalalgMdimTest(gdaltest.TestCase):

  def test_run_error(self) -> None:
    with self.assertRaisesRegex(
        RuntimeError, "method should not be called directly"
    ):
      gdal.Run(["mdim"])

  def test_drivers(self) -> None:
    with gdal.Run(["mdim"], drivers=True) as alg:
      names = [x["short_name"] for x in alg.Output()]
      self.assertIn("VRT", names)
      self.assertNotIn("GTiff", names)
      self.assertNotIn("ESRI Shapefile", names)


if __name__ == "__main__":
  absltest.main()
