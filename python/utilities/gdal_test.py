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

"""Tests for the main `gdal` CLI binary and Python `gdal.Run`/`Algorithm` API.

Ported from upstream autotest/utilities/test_gdal.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


class GdalCliTest(gdaltest.TestCase, parameterized.TestCase):

  def test_no_arg_help_json_invalid_and_format(self) -> None:
    out, err, code = utilities_util.run_gdal_cli([])
    self.assertEqual(code, 1)
    self.assertEqual(out, "")
    self.assertIn("gdal: Missing command name", err)
    self.assertIn("Usage: ", err)

    out, err, code = utilities_util.run_gdal_cli(["--help"])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: "))
    self.assertEqual(err, "")

    out, err, code = utilities_util.run_gdal_cli(["--json-usage"])
    self.assertEqual(code, 0)
    self.assertEqual(err, "")
    self.assertIn("description", json.loads(out))

    out, err, code = utilities_util.run_gdal_cli(["--invalid"])
    self.assertEqual(code, 1)
    self.assertEqual(out, "")
    self.assertIn("Option '--invalid' is unknown", err)
    self.assertIn("Usage: ", err)

    out, err, code = utilities_util.run_gdal_cli(["--format", "MEM"])
    self.assertEqual(code, 0)
    self.assertIn("Short Name: MEM", out)
    self.assertEqual(err, "")

  def test_format_output_progress_and_quiet(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_path = os.path.join(tmpdir, "out.xxx")
      out, err, code = utilities_util.run_gdal_cli(
          ["raster", "convert", "--format", "GTiff", byte_tif, out_path]
      )
      self.assertEqual(code, 0)
      self.assertEqual(err, "")
      self.assertTrue(out.startswith("0...10...20..."))
      self.assertIsNotNone(gdal.VSIStatL(out_path))

      out_q_path = os.path.join(tmpdir, "out_q.xxx")
      out_q, err_q, code_q = utilities_util.run_gdal_cli(
          ["raster", "convert", "-q", "--format", "GTiff", byte_tif, out_q_path]
      )
      self.assertEqual(code_q, 0)
      self.assertEqual(err_q, "")
      self.assertFalse(out_q.startswith("0...10...20..."))
      self.assertIsNotNone(gdal.VSIStatL(out_q_path))

  def test_failures_and_suggestions(self) -> None:
    out, err, code = utilities_util.run_gdal_cli(
        ["raster", "info", "i_do_not_exist"]
    )
    self.assertEqual(code, 1)
    self.assertEqual(out, "")
    self.assertIn("i_do_not_exist:", err)
    self.assertIn("Usage: ", err)

    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    out, err, code = utilities_util.run_gdal_cli([utmsmall])
    self.assertEqual(code, 0)
    self.assertEqual(err, "")
    self.assertTrue(out.startswith("Driver: GTiff/GeoTIFF"))

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    _, _, code = utilities_util.run_gdal_cli([
        "raster",
        "convert",
        byte_tif,
        "/vsimem/out.tif||maxlength=100",
    ])
    self.assertEqual(code, 1)

    for args, snippet in (
        (["rastr"], "Algorithm 'rastr' is unknown. Do you mean 'raster'?"),
        (
            ["raster", "nifo"],
            "Algorithm 'nifo' is unknown. Do you mean 'info'?",
        ),
        (
            ["raster", "info", "--frmt=json"],
            "Option '--frmt' is unknown. Do you mean '--format'?",
        ),
    ):
      _, err, code = utilities_util.run_gdal_cli(args)
      self.assertEqual(code, 1)
      self.assertIn(snippet, err)

  def test_config_not_serialized_to_gdalg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_json = os.path.join(tmpdir, "out.gdalg.json")
      _, _, code = utilities_util.run_gdal_cli(
          ["raster", "reproject", "--config", "FOO=BAR", byte_tif, out_json]
      )
      self.assertEqual(code, 0)
      with open(out_json, "r", encoding="utf-8") as f:
        parsed = json.load(f)
      self.assertIn("gdal_version", parsed)
      del parsed["gdal_version"]
      self.assertEqual(
          parsed,
          {
              "command_line": (
                  f"gdal raster reproject --input {byte_tif}"
                  " --output-format stream --output streamed_dataset"
              ),
              "type": "gdal_streamed_alg",
          },
      )

  def test_completion_basics_co_lco_oo_dst_crs_and_config(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(["completion", "gdal"])
    self.assertEqual(code, 0)
    tokens = out.split(" ")
    for expected in ("convert", "info", "raster", "vector"):
      self.assertIn(expected, tokens)

    out, _, _ = utilities_util.run_gdal_cli(["completion", "gdal", "raster"])
    for expected in ("convert", "edit", "info", "reproject", "pipeline"):
      self.assertIn(expected, out.split(" "))

    out, _, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "info", "--form"]
    )
    self.assertEqual(out.split(" "), ["--format"])

    out, _, _ = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "raster",
        "convert",
        "--config",
        "GDAL_SKIP=GTiff",
        "--of",
    ])
    self.assertNotIn("GTiff", out.split(" "))

    byte_zip = utilities_util.get_testdata_path("../gcore/data/byte.tif.zip")
    out, _, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "convert", f"/vsizip/{byte_zip}/"]
    )
    self.assertEqual(out.split(" "), [f"/vsizip/{byte_zip}/byte.tif"])

    out, _, _ = utilities_util.run_gdal_cli([
        "completion",
        "gdal",
        "raster",
        "convert",
        "in.tif",
        "out.tif",
        "--co",
        "TILED=",
    ])
    self.assertEqual(out.split(" "), ["NO", "YES"])

    out, _, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "reproject", "--dst-crs", "EPSG:4326"]
    )
    self.assertEqual(out, "4326")

    out, err, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "convert", "--config", "CPL_"]
    )
    self.assertIn("CPL_DEBUG=", out)
    self.assertEqual(err, "")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    out, _, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "vector", "info", poly_shp, "--layer"]
    )
    self.assertEqual(out.split(" "), ["poly"])

  @parameterized.parameters(None, "raster", "vector")
  def test_completion_pipeline(self, subcommand: str | None) -> None:
    base = ["completion", "gdal"]
    if subcommand:
      base.append(subcommand)
    base.append("pipeline")

    out, _, code = utilities_util.run_gdal_cli(base)
    self.assertEqual(code, 0)
    tokens = out.split(" ")
    self.assertIn("read", tokens)
    self.assertNotIn("write", tokens)
    if subcommand == "raster":
      self.assertIn("calc", tokens)
      self.assertNotIn("concat", tokens)
    elif subcommand == "vector":
      self.assertNotIn("calc", tokens)
      self.assertIn("concat", tokens)

  def test_question_mark_and_algorithm_api(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    _, err, _ = utilities_util.run_gdal_cli(
        ["vector", "info", poly_shp, "--layer=?"]
    )
    self.assertIn(
        "Single potential value for argument 'input-layer' is 'poly'", err
    )

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with self.assertRaisesRegex(RuntimeError, "not a valid sub-algorithm"):
      _ = gdal.GetGlobalAlgorithmRegistry()["raster"]["not_existing"]

    with self.assertRaisesRegex(RuntimeError, "not a valid algorithm"):
      gdal.Algorithm("i_do_not_exist")

    with gdal.Algorithm("raster info") as alg:
      self.assertEqual(alg.GetName(), "info")

    with gdal.Run("raster", "info", {"input": byte_tif}) as alg:
      self.assertLen(alg.Output()["bands"], 1)

    with gdal.Run(
        ["gdal", "raster", "reproject"],
        input=byte_tif,
        output_format="MEM",
        dst_crs="EPSG:4326",
    ) as alg:
      self.assertEqual(
          alg.Output().GetSpatialRef().GetAuthorityCode(None), "4326"
      )

    with gdal.Run([], drivers=True) as alg:
      names = [x["short_name"] for x in alg.Output()]
      self.assertIn("GTiff", names)
      self.assertIn("ESRI Shapefile", names)


if __name__ == "__main__":
  absltest.main()
