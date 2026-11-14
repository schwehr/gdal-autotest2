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

"""Tests for `gdal vsi copy`.

Ported from upstream autotest/utilities/test_gdalalg_vsi_copy.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vsi"]["copy"]


def _create_sample_tree(root_dir: str) -> str:
  src_dir = f"{root_dir}/src"
  gdal.Mkdir(src_dir, 0o755)
  gdal.FileFromMemBuffer(f"{src_dir}/a", "foo")
  gdal.Mkdir(f"{src_dir}/subdir", 0o755)
  gdal.FileFromMemBuffer(f"{src_dir}/subdir/b", "bar")
  return src_dir


class GdalalgVsiCopyTest(gdaltest.TestCase):

  def test_single_dir_and_file_destination(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("vsi_copy_single") as tmp:
      alg = _get_alg()
      alg["source"] = byte_tif
      alg["destination"] = tmp
      self.assertTrue(alg.Run())
      self.assertEqual(gdal.VSIStatL(f"{tmp}/byte.tif").size, 736)

      alg = _get_alg()
      alg["source"] = byte_tif
      alg["destination"] = f"{tmp}/out.tif"
      self.assertTrue(alg.Run())
      self.assertEqual(gdal.VSIStatL(f"{tmp}/out.tif").size, 736)

  def test_single_progress(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    last_pct = [0.0]

    def my_progress(pct: float, _msg: str, _user_data: object) -> bool:
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("vsi_copy_progress") as tmp:
      alg = _get_alg()
      alg["source"] = byte_tif
      alg["destination"] = tmp
      self.assertTrue(alg.Run(my_progress))
      self.assertEqual(last_pct[0], 1.0)
      self.assertEqual(gdal.VSIStatL(f"{tmp}/byte.tif").size, 736)

  def test_source_errors(self) -> None:
    alg = _get_alg()
    alg["source"] = "/i_do/not/exist.bin"
    alg["destination"] = "/vsimem/"
    with self.assertRaisesRegex(RuntimeError, "cannot be accessed"):
      alg.Run()

    gcore_dir = os.path.dirname(
        utilities_util.get_testdata_path("../gcore/data/byte.tif")
    )
    alg = _get_alg()
    alg["source"] = gcore_dir
    alg["destination"] = "/vsimem/"
    with self.assertRaisesRegex(RuntimeError, "is a directory"):
      alg.Run()

  def test_source_does_not_exist_vsi(self) -> None:
    if not utilities_util.has_curl():
      self.skipTest("CURL not available")
    for source, recursive in (
        ("/vsioss/i_do_not/exist.bin", False),
        ("/vsioss/i_do_not/exist.bin", True),
        ("/vsioss/i_do_not/exist.bin/*", True),
    ):
      with gdal.config_option("OSS_SECRET_ACCESS_KEY", ""):
        alg = _get_alg()
        alg["source"] = source
        alg["destination"] = "/vsimem/"
        if recursive:
          alg["recursive"] = True
        with self.assertRaisesRegex(RuntimeError, "InvalidCredentials"):
          alg.Run()

  def test_recursive_destination_does_not_exist(self) -> None:
    last_pct = [0.0]

    def my_progress(pct: float, _msg: str, _user_data: object) -> bool:
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("vsi_copy_rec_new") as tmp:
      src_dir = _create_sample_tree(tmp)
      dst_dir = f"{tmp}/dst"
      alg = _get_alg()
      alg["source"] = src_dir
      alg["destination"] = dst_dir
      alg["recursive"] = True
      self.assertTrue(alg.Run(my_progress))
      self.assertEqual(last_pct[0], 1.0)
      self.assertEqual(
          set(gdal.ReadDirRecursive(dst_dir)),
          set(gdal.ReadDirRecursive(src_dir)),
      )

  def test_recursive_destination_exists(self) -> None:
    with utilities_util.vsimem_dir("vsi_copy_rec_exists") as tmp:
      src_dir = _create_sample_tree(tmp)
      dst_dir = f"{tmp}/dst"
      gdal.Mkdir(dst_dir, 0o755)

      alg = _get_alg()
      alg["source"] = src_dir
      alg["destination"] = dst_dir
      alg["recursive"] = True
      self.assertTrue(alg.Run())
      res = set(gdal.ReadDirRecursive(dst_dir))
      res.remove("src/")
      self.assertEqual(
          {x[len("src/") :] for x in res},
          set(gdal.ReadDirRecursive(src_dir)),
      )

  def test_source_ends_slash_star(self) -> None:
    with utilities_util.vsimem_dir("vsi_copy_star_rec") as tmp:
      src_dir = _create_sample_tree(tmp)
      dst_dir = f"{tmp}/dst"
      alg = _get_alg()
      alg["source"] = f"{src_dir}/*"
      alg["destination"] = dst_dir
      alg["recursive"] = True
      self.assertTrue(alg.Run())
      self.assertEqual(
          set(gdal.ReadDirRecursive(dst_dir)),
          set(gdal.ReadDirRecursive(src_dir)),
      )

    with utilities_util.vsimem_dir("vsi_copy_star_flat") as tmp:
      src_dir = _create_sample_tree(tmp)
      dst_dir = f"{tmp}/dst"
      alg = _get_alg()
      alg["source"] = f"{src_dir}/*"
      alg["destination"] = dst_dir
      self.assertTrue(alg.Run())
      self.assertEqual(set(gdal.ReadDirRecursive(dst_dir)), {"a", "subdir/"})

  def test_recursive_destination_cannot_be_created(self) -> None:
    with utilities_util.vsimem_dir("vsi_copy_mkdir_fail") as tmp:
      src_dir = _create_sample_tree(tmp)
      alg = _get_alg()
      alg["source"] = src_dir
      alg["destination"] = "/i_do/not/exist"
      alg["recursive"] = True
      with self.assertRaisesRegex(
          RuntimeError, "Cannot create directory /i_do/not/exist"
      ):
        alg.Run()

      alg = _get_alg()
      alg["source"] = src_dir
      alg["destination"] = "/i_do/not/exist"
      alg["recursive"] = True
      alg["skip-errors"] = True
      with gdal.quiet_errors():
        self.assertTrue(alg.Run())


if __name__ == "__main__":
  absltest.main()
