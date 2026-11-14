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

"""Tests for `gdal vsi list`.

Ported from upstream autotest/utilities/test_gdalalg_vsi_list.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os
from typing import Any

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vsi"]["list"]


def _del_last_modification_date(obj: Any) -> None:
  if isinstance(obj, list):
    for item in obj:
      _del_last_modification_date(item)
  elif isinstance(obj, dict):
    obj.pop("last_modification_date", None)
    for val in obj.values():
      _del_last_modification_date(val)


class GdalalgVsiListTest(gdaltest.TestCase):

  def test_vsi_list_vsimem(self) -> None:
    with utilities_util.vsimem_dir("vsi_list_test") as vsimem:
      alg = _get_alg()
      alg["filename"] = f"{vsimem}/i_do_not_exist"
      with self.assertRaises(RuntimeError):
        alg.Run()

      alg = _get_alg()
      alg["filename"] = vsimem
      self.assertTrue(alg.Run())
      self.assertEqual(json.loads(alg["output-string"]), [])

      gdal.FileFromMemBuffer(f"{vsimem}/a", "a")
      gdal.Mkdir(f"{vsimem}/subdir", 0o755)
      gdal.FileFromMemBuffer(f"{vsimem}/subdir/b", "b")
      gdal.FileFromMemBuffer(f"{vsimem}/subdir/c", "c")
      gdal.Mkdir(f"{vsimem}/subdir/subsubdir", 0o755)
      gdal.FileFromMemBuffer(f"{vsimem}/d", "d")

      alg = _get_alg()
      alg["filename"] = vsimem
      self.assertTrue(alg.Run())
      self.assertEqual(json.loads(alg["output-string"]), ["a", "d", "subdir"])

      alg = _get_alg()
      alg["filename"] = f"{vsimem}/a"
      self.assertTrue(alg.Run())
      self.assertEqual(json.loads(alg["output-string"]), f"{vsimem}/a")

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      self.assertTrue(alg.Run())
      self.assertEqual(
          json.loads(alg["output-string"]),
          ["a", "d", "subdir", "subdir/b", "subdir/c", "subdir/subsubdir"],
      )

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      alg["depth"] = 1
      self.assertTrue(alg.Run())
      self.assertEqual(json.loads(alg["output-string"]), ["a", "d", "subdir"])

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      alg["depth"] = 2
      self.assertTrue(alg.Run())
      self.assertEqual(
          json.loads(alg["output-string"]),
          ["a", "d", "subdir", "subdir/b", "subdir/c", "subdir/subsubdir"],
      )

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["long-listing"] = True
      self.assertTrue(alg.Run())
      parsed = json.loads(alg["output-string"])
      _del_last_modification_date(parsed)
      self.assertEqual(
          parsed,
          [
              {
                  "name": "a",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "d",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "subdir",
                  "permissions": "d---------",
                  "size": 0,
                  "type": "directory",
              },
          ],
      )

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      alg["long-listing"] = True
      self.assertTrue(alg.Run())
      parsed = json.loads(alg["output-string"])
      _del_last_modification_date(parsed)
      self.assertEqual(
          parsed,
          [
              {
                  "name": "a",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "d",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "subdir",
                  "permissions": "d---------",
                  "size": 0,
                  "type": "directory",
              },
              {
                  "name": "subdir/b",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "subdir/c",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "subdir/subsubdir",
                  "permissions": "d---------",
                  "size": 0,
                  "type": "directory",
              },
          ],
      )

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      alg["long-listing"] = True
      alg["tree"] = True
      self.assertTrue(alg.Run())
      parsed = json.loads(alg["output-string"])
      _del_last_modification_date(parsed)
      self.assertEqual(
          parsed,
          [
              {
                  "name": "a",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "d",
                  "permissions": "----------",
                  "size": 1,
                  "type": "file",
              },
              {
                  "name": "subdir",
                  "permissions": "d---------",
                  "size": 0,
                  "type": "directory",
                  "entries": [
                      {
                          "name": "b",
                          "permissions": "----------",
                          "size": 1,
                          "type": "file",
                      },
                      {
                          "name": "c",
                          "permissions": "----------",
                          "size": 1,
                          "type": "file",
                      },
                      {
                          "entries": [],
                          "name": "subsubdir",
                          "permissions": "d---------",
                          "size": 0,
                          "type": "directory",
                      },
                  ],
              },
          ],
      )

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      alg["tree"] = True
      self.assertTrue(alg.Run())
      parsed = json.loads(alg["output-string"])
      _del_last_modification_date(parsed)
      self.assertEqual(
          parsed,
          [
              "a",
              "d",
              {
                  "name": "subdir",
                  "entries": [
                      "b",
                      "c",
                      {"entries": [], "name": "subsubdir"},
                  ],
              },
          ],
      )

      alg = _get_alg()
      alg["filename"] = vsimem
      alg["recursive"] = True
      alg["format"] = "text"
      alg["absolute-path"] = True
      self.assertTrue(alg.Run())
      self.assertEqual(
          alg["output-string"][:-1].split("\n"),
          [
              f"{vsimem}/{name}"
              for name in (
                  "a",
                  "d",
                  "subdir",
                  "subdir/b",
                  "subdir/c",
                  "subdir/subsubdir",
              )
          ],
      )

  def test_vsi_list_local_filesystem(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    data_dir = os.path.dirname(utmsmall)

    alg = _get_alg()
    alg["filename"] = data_dir
    self.assertTrue(alg.Run())
    self.assertIn("utmsmall.tif", json.loads(alg["output-string"]))

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      sub_data = os.path.join(tmpdir, "data")
      os.mkdir(sub_data)
      with open(os.path.join(sub_data, "utmsmall.tif"), "wb") as f:
        f.write(b"x")

      orig_cwd = os.getcwd()
      os.chdir(tmpdir)
      try:
        alg = _get_alg()
        alg["filename"] = "."
        self.assertTrue(alg.Run())

        alg = _get_alg()
        alg["filename"] = "data"
        alg["absolute-path"] = True
        self.assertTrue(alg.Run())
        expected = os.path.join(os.getcwd(), "data", "utmsmall.tif").replace(
            "\\", "/"
        )
        self.assertIn(
            expected,
            [x.replace("\\", "/") for x in json.loads(alg["output-string"])],
        )

        alg = _get_alg()
        alg["filename"] = "data"
        alg["long-listing"] = True
        alg["format"] = "text"
        self.assertTrue(alg.Run())
        self.assertIn("unknown unknown", alg["output-string"])
        self.assertIn("utmsmall.tif", alg["output-string"])
      finally:
        os.chdir(orig_cwd)

  def test_source_does_not_exist_vsi(self) -> None:
    if not utilities_util.has_curl():
      self.skipTest("libcurl/HTTP support not available")
    with gdal.config_option("OSS_SECRET_ACCESS_KEY", ""):
      alg = _get_alg()
      alg["filename"] = "/vsioss/i_do_not/exist.bin"
      with self.assertRaisesRegex(RuntimeError, "InvalidCredentials"):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
