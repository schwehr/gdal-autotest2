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

"""Tests for `gdal vsi sozip`.

Ported from upstream autotest/utilities/test_gdalalg_vsi_sozip.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os
import sys
from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _create_source_files(vsimem: str) -> None:
  gdal.FileFromMemBuffer(f"{vsimem}/a", b"x" * (1024 * 1024 + 1))
  gdal.FileFromMemBuffer(f"{vsimem}/b", b"x" * 40000)
  gdal.Mkdir(f"{vsimem}/subdir", 0o755)
  gdal.FileFromMemBuffer(f"{vsimem}/subdir/c", b"x")


class GdalalgVsiSozipTest(gdaltest.TestCase, parameterized.TestCase):

  def test_sozip_parent_cannot_run_directly(self) -> None:
    with self.assertRaisesRegex(RuntimeError, "should not be called directly"):
      gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"].Run()

  def test_create_no_zip_extension(self) -> None:
    alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
    with self.assertRaisesRegex(
        RuntimeError, r"Extension of zip filename should be \.zip"
    ):
      alg["output"] = "foo"

  @parameterized.parameters("auto", "yes", "no")
  def test_create_and_validate(self, enable_sozip: str) -> None:
    with utilities_util.vsimem_dir("vsi_sozip_create") as vsimem:
      _create_source_files(vsimem)

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
      alg["input"] = f"{vsimem}/b"
      alg["output"] = f"{vsimem}/out.zip"
      alg["quiet"] = True
      alg["no-paths"] = True
      alg["enable-sozip"] = enable_sozip
      alg["sozip-chunk-size"] = 16384
      self.assertTrue(alg.Run())
      self.assertEqual(alg["output-string"], "")

      self.assertEqual(gdal.VSIStatL(f"/vsizip/{vsimem}/out.zip/b").size, 40000)

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["validate"]
      alg["input"] = f"{vsimem}/out.zip"
      self.assertTrue(alg.Run())
      if enable_sozip == "yes":
        self.assertIn(
            "b has a valid SOZip index, using chunk_size = 16384",
            alg["output-string"],
        )
        self.assertIn(
            "is a valid .zip file, and contains 1 SOZip-enabled file(s)",
            alg["output-string"],
        )
      else:
        self.assertNotIn("has a valid SOZip index", alg["output-string"])
        self.assertIn(
            "is a valid .zip file, but does not contain any SOZip-enabled"
            " files.",
            alg["output-string"],
        )

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
      alg["input"] = f"{vsimem}/subdir/c"
      alg["output"] = f"{vsimem}/out.zip"
      alg["no-paths"] = True
      alg["content-type"] = "application/octet-stream"
      self.assertTrue(alg.Run())

      self.assertEqual(gdal.VSIStatL(f"/vsizip/{vsimem}/out.zip/b").size, 40000)
      self.assertEqual(gdal.VSIStatL(f"/vsizip/{vsimem}/out.zip/c").size, 1)

      md = gdal.GetFileMetadata(f"/vsizip/{vsimem}/out.zip/b", "ZIP")
      if enable_sozip == "yes":
        self.assertEqual(md["SOZIP_VALID"], "YES")
        self.assertEqual(md["SOZIP_CHUNK_SIZE"], "16384")
      else:
        self.assertNotIn("SOZIP_VALID", md)

      md = gdal.GetFileMetadata(f"/vsizip/{vsimem}/out.zip/c", None)
      self.assertEqual(md["Content-Type"], "application/octet-stream")

  def test_create_non_existing_input(self) -> None:
    with utilities_util.vsimem_dir("vsi_sozip_missing") as vsimem:
      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
      alg["input"] = f"{vsimem}/non_existing"
      alg["output"] = f"{vsimem}/out.zip"
      with self.assertRaisesRegex(RuntimeError, "does not exist"):
        alg.Run()

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del pct, msg, user_data
        return True

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
      alg["input"] = f"{vsimem}/non_existing"
      alg["output"] = f"{vsimem}/out.zip"
      with self.assertRaisesRegex(RuntimeError, "does not exist"):
        alg.Run(my_progress)

  def test_create_input_is_directory(self) -> None:
    with utilities_util.vsimem_dir("vsi_sozip_dir") as vsimem:
      gdal.Mkdir(f"{vsimem}/dir", 0o755)
      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
      alg["input"] = f"{vsimem}/dir"
      alg["output"] = f"{vsimem}/out.zip"
      with self.assertRaisesRegex(RuntimeError, "is a directory"):
        alg.Run()

  def test_create_failed_adding(self) -> None:
    if sys.platform != "linux":
      self.skipTest("not linux")
    if os.getuid() == 0:
      self.skipTest("running as root... skipping")

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      input_filename = os.path.join(tmpdir, "cannot_read")
      with open(input_filename, "wb"):
        pass
      os.chmod(input_filename, 0)
      try:
        alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
        alg["input"] = input_filename
        alg["output"] = os.path.join(tmpdir, "out.zip")
        with self.assertRaisesRegex(
            RuntimeError, f"Failed adding {input_filename}"
        ):
          alg.Run()
      finally:
        os.chmod(input_filename, 0o600)

  def test_create_recursive_and_optimize_and_validate(self) -> None:
    with utilities_util.vsimem_dir("vsi_sozip_rec") as vsimem:
      _create_source_files(vsimem)

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["create"]
      alg["input"] = vsimem
      alg["recursive"] = True
      alg["output"] = f"{vsimem}/out.zip"

      last_pct = [0.0]

      def my_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      self.assertTrue(alg.Run(my_progress))
      self.assertEqual(last_pct[0], 1.0)
      self.assertEqual(
          alg["output-string"],
          f"Adding {vsimem}/a... (1/3)\n"
          f"Adding {vsimem}/b... (2/3)\n"
          f"Adding {vsimem}/subdir/c... (3/3)\n",
      )

      self.assertEqual(
          gdal.VSIStatL(f"/vsizip/{vsimem}/out.zip{vsimem}/a").size,
          1024 * 1024 + 1,
      )
      self.assertEqual(
          gdal.VSIStatL(f"/vsizip/{vsimem}/out.zip{vsimem}/b").size, 40000
      )
      self.assertEqual(
          gdal.VSIStatL(f"/vsizip/{vsimem}/out.zip{vsimem}/subdir/c").size, 1
      )

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["optimize"]
      alg["input"] = f"{vsimem}/out.zip"
      alg["output"] = f"{vsimem}/out2.zip"
      self.assertTrue(alg.Run())

      self.assertEqual(
          gdal.VSIStatL(f"/vsizip/{vsimem}/out2.zip{vsimem}/a").size,
          1024 * 1024 + 1,
      )
      self.assertEqual(
          gdal.VSIStatL(f"/vsizip/{vsimem}/out2.zip{vsimem}/b").size, 40000
      )
      self.assertEqual(
          gdal.VSIStatL(f"/vsizip/{vsimem}/out2.zip{vsimem}/subdir/c").size, 1
      )

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["optimize"]
      alg["input"] = f"{vsimem}/out.zip"
      alg["output"] = f"{vsimem}/out2.zip"
      with self.assertRaisesRegex(
          RuntimeError, r"already exists\. Use --overwrite"
      ):
        alg.Run()

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["optimize"]
      alg["input"] = f"{vsimem}/i_do_not_exist.zip"
      alg["output"] = f"{vsimem}/out3.zip"
      with self.assertRaisesRegex(RuntimeError, r"is not a valid \.zip file"):
        alg.Run()

      alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["validate"]
      alg["input"] = f"{vsimem}/out.zip"
      self.assertTrue(alg.Run())
      self.assertIn(
          "a has a valid SOZip index, using chunk_size = 32768",
          alg["output-string"],
      )
      self.assertIn(
          "is a valid .zip file, and contains 1 SOZip-enabled file(s)",
          alg["output-string"],
      )

  def test_list_and_validate_not_a_zip(self) -> None:
    alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["list"]
    alg["input"] = "/i_do/not/exist.zip"
    with self.assertRaisesRegex(RuntimeError, r"is not a valid \.zip file"):
      alg.Run()

    alg = gdal.GetGlobalAlgorithmRegistry()["vsi"]["sozip"]["validate"]
    alg["input"] = "/i_do/not/exist.zip"
    with self.assertRaisesRegex(RuntimeError, r"is not a valid \.zip file"):
      alg.Run()


if __name__ == "__main__":
  absltest.main()
