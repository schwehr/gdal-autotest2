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

"""Tests for `gdal dataset identify`.

Ported from upstream autotest/utilities/test_gdalalg_dataset_identify.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os
from typing import Any

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


class GdalalgDatasetIdentifyTest(gdaltest.TestCase):

  def test_json(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run("dataset", "identify", filename=byte_tif) as alg:
      self.assertEqual(alg.Output(), [{"driver": "GTiff", "name": byte_tif}])

  def test_several_files_json_with_progress(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    uint16_tif = utilities_util.get_testdata_path("../gcore/data/uint16.tif")
    last_pct = [0.0]

    def on_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, last_pct[0])
      last_pct[0] = pct
      return True

    with gdal.Run(
        "dataset",
        "identify",
        filename=[byte_tif, uint16_tif],
        progress=on_progress,
    ) as alg:
      self.assertEqual(
          alg.Output(),
          [
              {"driver": "GTiff", "name": byte_tif},
              {"driver": "GTiff", "name": uint16_tif},
          ],
      )

    self.assertEqual(last_pct[0], 1.0)

  def test_cli_stdout(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    out, err, code = utilities_util.run_gdal_cli(
        ["dataset", "identify", utmsmall]
    )
    self.assertEqual(code, 0)
    self.assertEqual(err, "")
    self.assertTrue(out.startswith(f"{utmsmall}: GTiff"))

  def test_cli_completion(self) -> None:
    whiteblackred = utilities_util.get_testdata_path("data/whiteblackred.tif")
    prefix = whiteblackred[: -len(".tif")]
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "dataset", "identify", prefix]
    )
    self.assertEqual(code, 0)
    self.assertEqual(out.replace("\\", "/"), whiteblackred)

  def test_text(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run(
        "dataset", "identify", filename=byte_tif, format="text"
    ) as alg:
      self.assertEqual(alg.Output(), f"{byte_tif}: GTiff\n")

  def test_report_failures_json(self) -> None:
    with gdal.Run(
        "dataset",
        "identify",
        filename="/i_do/not/exist",
        report_failures=True,
    ) as alg:
      self.assertEqual(
          alg.Output(), [{"driver": None, "name": "/i_do/not/exist"}]
      )

  def test_report_failures_text(self) -> None:
    with gdal.Run(
        "dataset",
        "identify",
        filename="/i_do/not/exist",
        report_failures=True,
        format="text",
    ) as alg:
      self.assertEqual(alg.Output(), "/i_do/not/exist: unrecognized\n")

  def test_recursive(self) -> None:
    testdata_dir = os.path.dirname(
        utilities_util.get_testdata_path("data/utmsmall.tif")
    )
    with gdal.Run(
        "dataset", "identify", filename=testdata_dir, recursive=True
    ) as alg:
      self.assertEqual(
          alg.Output(),
          [{"name": testdata_dir, "driver": "ESRI Shapefile"}],
      )

  def test_force_recursive(self) -> None:
    testdata_dir = os.path.dirname(
        utilities_util.get_testdata_path("data/utmsmall.tif")
    )
    last_pct = [0.0]

    def on_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, last_pct[0])
      last_pct[0] = pct
      return True

    with gdal.Run(
        "dataset",
        "identify",
        filename=testdata_dir,
        force_recursive=True,
        progress=on_progress,
    ) as alg:
      self.assertGreater(len(alg.Output()), 1)

    self.assertEqual(last_pct[0], 1.0)


if __name__ == "__main__":
  absltest.main()
