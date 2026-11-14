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

"""Tests for `gdal vsi sync`.

Ported from upstream autotest/utilities/test_gdalalg_vsi_sync.py.

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


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vsi"]["sync"]


class GdalalgVsiSyncTest(gdaltest.TestCase):

  def test_nominal(self) -> None:
    with (
        utilities_util.vsimem_dir("vsi_sync_nominal") as vsi_dir,
        gcore_util.TestTemporaryDirectory() as tmpdir,
    ):
      src_path = f"{vsi_dir}/file.bin"
      dst_path = os.path.join(tmpdir, "dest.bin")
      gdal.FileFromMemBuffer(src_path, "foo")

      alg = _get_alg()
      alg["source"] = src_path
      alg["destination"] = dst_path
      self.assertTrue(alg.Run())
      self.assertEqual(gdal.VSIStatL(dst_path).size, 3)

  def test_source_does_not_exist(self) -> None:
    with utilities_util.vsimem_dir("vsi_sync_missing") as vsi_dir:
      alg = _get_alg()
      alg["source"] = f"{vsi_dir}/i_do_not_exist.bin"
      alg["destination"] = f"{vsi_dir}/dest.bin"
      with self.assertRaisesRegex(RuntimeError, "does not exist"):
        alg.Run()

  def test_destination_error(self) -> None:
    with (
        utilities_util.vsimem_dir("vsi_sync_err") as vsi_dir,
        gcore_util.TestTemporaryDirectory() as tmpdir,
    ):
      src_path = f"{vsi_dir}/file.bin"
      gdal.FileFromMemBuffer(src_path, "foo")

      alg = _get_alg()
      alg["source"] = src_path
      alg["destination"] = os.path.join(
          tmpdir, "i_do", "not", "exist", "dest.bin"
      )
      with self.assertRaisesRegex(RuntimeError, "Cannot create"):
        alg.Run()

  def test_source_does_not_exist_vsi(self) -> None:
    if not utilities_util.has_curl():
      self.skipTest("CURL not available")
    with gdal.config_option("OSS_SECRET_ACCESS_KEY", ""):
      alg = _get_alg()
      alg["source"] = "/vsioss/i_do_not/exist.bin"
      alg["destination"] = "/vsimem/"
      with self.assertRaisesRegex(RuntimeError, "InvalidCredentials"):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
