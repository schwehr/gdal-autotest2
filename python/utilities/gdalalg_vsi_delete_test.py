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

"""Tests for `gdal vsi delete`.

Ported from upstream autotest/utilities/test_gdalalg_vsi_delete.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os
import sys

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vsi"]["delete"]


class GdalalgVsiDeleteTest(gdaltest.TestCase):

  def test_delete_file(self) -> None:
    with utilities_util.vsimem_dir("vsi_delete_test") as tmp:
      target = f"{tmp}/test"
      gdal.FileFromMemBuffer(target, "test")

      alg = _get_alg()
      alg["filename"] = target
      self.assertTrue(alg.Run())
      self.assertIsNone(gdal.VSIStatL(target))

  def test_delete_file_not_existing(self) -> None:
    alg = _get_alg()
    alg["filename"] = "/i_do/not/exist"
    with self.assertRaisesRegex(RuntimeError, "does not exist"):
      alg.Run()

  def test_delete_dir(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      subdir = os.path.join(tmpdir, "subdir")
      gdal.Mkdir(subdir, 0o755)

      alg = _get_alg()
      alg["filename"] = subdir
      self.assertTrue(alg.Run())
      self.assertIsNone(gdal.VSIStatL(subdir))

  def test_delete_file_failed(self) -> None:
    if sys.platform == "win32":
      self.skipTest("incompatible platform")
    if os.getuid() == 0:
      self.skipTest("running as root")

    alg = _get_alg()
    alg["filename"] = "/dev/null"
    with self.assertRaisesRegex(RuntimeError, "Cannot delete /dev/null"):
      alg.Run()

  def test_delete_dir_recursive(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      subdir = os.path.join(tmpdir, "subdir")
      gdal.Mkdir(subdir, 0o755)
      with open(os.path.join(subdir, "file"), "wb"):
        pass

      alg = _get_alg()
      alg["filename"] = subdir
      alg["recursive"] = True
      self.assertTrue(alg.Run())
      self.assertIsNone(gdal.VSIStatL(subdir))

  def test_delete_source_does_not_exist_vsi(self) -> None:
    if not utilities_util.has_curl():
      self.skipTest("curl/HTTP support not available")

    with gdaltest.config_option("OSS_SECRET_ACCESS_KEY", ""):
      alg = _get_alg()
      alg["filename"] = "/vsioss/i_do_not/exist.bin"
      with self.assertRaisesRegex(RuntimeError, "InvalidCredentials"):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
