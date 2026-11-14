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

"""Tests for `gdal vsi move`.

Ported from upstream autotest/utilities/test_gdalalg_vsi_move.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_vsi_move_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vsi"]["move"]


class GdalalgVsiMoveTest(gdaltest.TestCase):

  def test_nominal(self) -> None:
    with (
        utilities_util.vsimem_dir("vsi_move_nominal") as vsimem_dir,
        gcore_util.TestTemporaryDirectory() as tmpdir,
    ):
      src_path = f"{vsimem_dir}/file.bin"
      dst_path = os.path.join(tmpdir, "dest.bin")
      gdal.FileFromMemBuffer(src_path, b"foo")

      alg = _get_vsi_move_alg()
      alg["source"] = src_path
      alg["destination"] = dst_path
      self.assertTrue(alg.Run())
      self.assertIsNone(gdal.VSIStatL(src_path))
      stat = gdal.VSIStatL(dst_path)
      self.assertIsNotNone(stat)
      self.assertEqual(stat.size, 3)

  def test_source_does_not_exist(self) -> None:
    with utilities_util.vsimem_dir("vsi_move_missing") as vsimem_dir:
      alg = _get_vsi_move_alg()
      alg["source"] = f"{vsimem_dir}/i_do_not_exist.bin"
      alg["destination"] = f"{vsimem_dir}/dest.bin"
      with self.assertRaisesRegex(RuntimeError, "does not exist"):
        alg.Run()

  def test_destination_error_preserves_source(self) -> None:
    with (
        utilities_util.vsimem_dir("vsi_move_err") as vsimem_dir,
        gcore_util.TestTemporaryDirectory() as tmpdir,
    ):
      src_path = f"{vsimem_dir}/file.bin"
      gdal.FileFromMemBuffer(src_path, b"foo")

      alg = _get_vsi_move_alg()
      alg["source"] = src_path
      alg["destination"] = os.path.join(
          tmpdir, "i_do", "not", "exist", "dest.bin"
      )
      with self.assertRaisesRegex(RuntimeError, "could not be moved to"):
        alg.Run()
      self.assertIsNotNone(gdal.VSIStatL(src_path))

  def test_source_does_not_exist_vsi_curl(self) -> None:
    if not utilities_util.has_curl():
      self.skipTest("libcurl/HTTP support not available")

    with gdaltest.config_option("OSS_SECRET_ACCESS_KEY", ""):
      alg = _get_vsi_move_alg()
      alg["source"] = "/vsioss/i_do_not/exist.bin"
      alg["destination"] = "/vsimem/"
      with self.assertRaisesRegex(RuntimeError, "InvalidCredentials"):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
