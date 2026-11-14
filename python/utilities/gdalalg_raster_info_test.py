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

"""Tests for `gdal raster info`.

Ported from upstream autotest/utilities/test_gdalalg_raster_info.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_info_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["info"]


class GdalalgRasterInfoTest(gdaltest.TestCase):

  def test_cli_stdout_text_and_json(self) -> None:
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    out, _, code = utilities_util.run_gdal_cli(["raster", "info", utmsmall])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Driver: GTiff/GeoTIFF"))

    out_json, _, code = utilities_util.run_gdal_cli(
        ["raster", "info", "--format", "json", utmsmall]
    )
    self.assertEqual(code, 0)
    parsed = json.loads(out_json)
    self.assertLen(parsed["bands"], 1)

  def test_text_mm_and_checksum(self) -> None:
    info = _get_info_alg()
    utmsmall = utilities_util.get_testdata_path("data/utmsmall.tif")
    self.assertTrue(
        info.ParseRunAndFinalize(
            ["--format=text", "--mm", "--checksum", utmsmall]
        )
    )
    output_string = info["output-string"]
    self.assertTrue(output_string.startswith("Driver: GTiff/GeoTIFF"))
    self.assertIn("Computed Min/Max=0.000,255.000", output_string)
    self.assertIn("Checksum=", output_string)

  def test_stats_approx_and_hist(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    for flag in ("--stats", "--approx-stats"):
      info = _get_info_alg()
      ds = gdal.Translate("", byte_tif, format="MEM")
      info["input"] = ds
      self.assertTrue(info.ParseRunAndFinalize([flag]))
      parsed = json.loads(info["output-string"])
      self.assertIn("stdDev", parsed["bands"][0])

    info = _get_info_alg()
    ds = gdal.Translate("", byte_tif, format="MEM")
    info["input"] = ds
    self.assertTrue(info.ParseRunAndFinalize(["--hist"]))
    parsed = json.loads(info["output-string"])
    self.assertIn("histogram", parsed["bands"][0])

  def test_exclusion_flags_and_metadata_domains(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    info = _get_info_alg()
    ds = gdal.Translate("", byte_tif, format="MEM")
    info["input"] = ds
    self.assertTrue(
        info.ParseRunAndFinalize([
            "--no-gcp",
            "--no-md",
            "--no-ct",
            "--no-fl",
            "--no-nodata",
            "--no-mask",
        ])
    )

    info_mdd = _get_info_alg()
    ds_mdd = gdal.Translate("", byte_tif, format="MEM")
    ds_mdd.SetMetadataItem("foo", "bar", "MY_DOMAIN")
    info_mdd["input"] = ds_mdd
    self.assertTrue(info_mdd.ParseRunAndFinalize(["--list-mdd"]))
    parsed = json.loads(info_mdd["output-string"])
    self.assertIn("MY_DOMAIN", parsed["metadata"]["metadataDomains"])

    info_all = _get_info_alg()
    info_all["input"] = ds_mdd
    self.assertTrue(info_all.ParseRunAndFinalize(["--mdd=all"]))
    parsed_all = json.loads(info_all["output-string"])
    self.assertEqual(parsed_all["metadata"]["MY_DOMAIN"], {"foo": "bar"})

  def test_subdataset_and_errors(self) -> None:
    subifds = utilities_util.get_testdata_path(
        "../gcore/data/tiff_with_subifds.tif"
    )
    info = _get_info_alg()
    self.assertTrue(
        info.ParseRunAndFinalize([f"--input={subifds}", "--subdataset=2"])
    )
    parsed = json.loads(info["output-string"])
    self.assertEqual(parsed["description"], f"GTIFF_DIR:2:{subifds}")

    with self.assertRaisesRegex(
        RuntimeError,
        "Invalid value for 'subdataset' argument. Should be between 1 and 2",
    ):
      _get_info_alg().ParseRunAndFinalize(
          [f"--input={subifds}", "--subdataset=3"]
      )

    mem_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    mem_ds.SetMetadataItem("SUBDATASET_1_DESC", "desc", "SUBDATASETS")
    mem_ds.SetMetadataItem("SUBDATASET_1_NAME", "i_do_not_exist", "SUBDATASETS")
    info_err = _get_info_alg()
    info_err["input"] = mem_ds
    with self.assertRaisesRegex(RuntimeError, "i_do_not_exist"):
      info_err.ParseRunAndFinalize(["--subdataset=1"])

  def test_read_gdalg_with_input_format(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    spec = {
        "type": "gdal_streamed_alg",
        "command_line": f"gdal raster pipeline ! read {byte_tif}",
    }
    with utilities_util.vsimem_dir("raster_info_gdalg") as tmp:
      tmp_gdalg = f"{tmp}/read_byte.gdalg.json"
      with gdal.VSIFile(tmp_gdalg, "wb") as out:
        out.write(json.dumps(spec).encode("utf-8"))
      info = _get_info_alg()
      info["input"] = tmp_gdalg
      info["input-format"] = "GDALG"
      self.assertTrue(info.Run())


if __name__ == "__main__":
  absltest.main()
