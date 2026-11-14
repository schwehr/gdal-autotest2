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

"""Tests for `materialize` step in `gdal raster pipeline`.

Ported from upstream autotest/utilities/test_gdalalg_raster_materialize.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os
import sys
from typing import Any

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _cleaned_listdir(path: str) -> list[str]:
  entries = gdal.ReadDir(path)
  if not entries:
    return []
  return [entry for entry in entries if entry not in (".", "..")]


class GdalalgRasterMaterializeTest(gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")

  def test_temp_output_tif(self) -> None:
    tab_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: Any) -> bool:
      del msg, user_data
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdaltest.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "raster",
            "pipeline",
            pipeline=(
                f"read {self.byte_tif} ! materialize"
                " ! write --of stream streamed_dataset"
            ),
            progress=my_progress,
        ) as alg:
          if sys.platform != "win32":
            self.assertEqual(_cleaned_listdir(tmpdir), [])
          self.assertEqual(tab_pct[0], 1.0)
          with alg.Output() as ds:
            self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      self.assertEqual(_cleaned_listdir(tmpdir), [])

  def test_temp_output_mem(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdaltest.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "raster",
            "pipeline",
            pipeline=(
                f"read {self.byte_tif} ! materialize --format MEM"
                " ! write --of stream streamed_dataset"
            ),
        ) as alg:
          self.assertEqual(_cleaned_listdir(tmpdir), [])
          with alg.Output() as ds:
            self.assertEqual(ds.GetDriver().GetDescription(), "MEM")
            self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      self.assertEqual(_cleaned_listdir(tmpdir), [])

  def test_temp_output_cog(self) -> None:
    if not utilities_util.has_driver("COG"):
      self.skipTest("COG driver not available")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdaltest.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "raster",
            "pipeline",
            pipeline=(
                f"read {self.byte_tif} ! materialize --format COG"
                " ! write --of stream streamed_dataset"
            ),
        ) as alg:
          if sys.platform != "win32":
            self.assertEqual(_cleaned_listdir(tmpdir), [])
          with alg.Output() as ds:
            self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      self.assertEqual(_cleaned_listdir(tmpdir), [])

  def test_temp_output_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      with gdaltest.config_option("CPL_TMPDIR", tmpdir):
        with gdal.Run(
            "raster",
            "pipeline",
            pipeline=(
                f"read {self.byte_tif} ! materialize --format GPKG"
                " ! write --of stream streamed_dataset"
            ),
        ) as alg:
          with alg.Output() as ds:
            self.assertEqual(ds.GetDriver().GetDescription(), "GPKG")
            self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      self.assertEqual(_cleaned_listdir(tmpdir), [])

  def test_manual_output(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_tif = os.path.join(tmpdir, "tmp.tif")
      with gdal.Run(
          "raster",
          "pipeline",
          pipeline=(
              f"read {self.byte_tif} ! materialize --output={out_tif}"
              " ! write --of stream streamed_dataset"
          ),
      ) as alg:
        with alg.Output() as ds:
          self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      with gdal.Open(out_tif) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_read_from_gdalg(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": f"gdal pipeline read {self.byte_tif} ! materialize",
    }
    with gdal.Open(json.dumps(spec)) as ds:
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_read_from_gdalg_error(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": (
            f"gdal pipeline read {self.byte_tif} ! materialize --output=illegal"
        ),
    }
    with self.assertRaisesRegex(
        RuntimeError,
        "Step 'materialize' not allowed in stream execution, unless the"
        " GDAL_ALGORITHM_ALLOW_WRITES_IN_STREAM configuration option is set",
    ):
      gdal.Open(json.dumps(spec))


if __name__ == "__main__":
  absltest.main()
