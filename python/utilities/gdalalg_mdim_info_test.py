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

"""Tests for `gdal mdim info`.

Ported from upstream autotest/utilities/test_gdalalg_mdim_info.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os
from typing import Any

from absl.testing import absltest
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_mdim_info_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["mdim"]["info"]


def _expected_byte_nc_summary() -> dict[str, Any]:
  return {
      "type": "group",
      "driver": "netCDF",
      "name": "/",
      "attributes": {
          "GDAL_AREA_OR_POINT": "Area",
          "Conventions": "CF-1.5",
          "GDAL": (
              "GDAL 3.8.0dev-refs/heads-dirty, released 2023/10/09"
              " (debug build)"
          ),
          "history": (
              "Mon Oct 09 18:27:35 2023: GDAL CreateCopy( byte.nc, ... )"
          ),
      },
      "dimensions": [
          {
              "name": "x",
              "full_name": "/x",
              "size": 20,
              "type": "HORIZONTAL_X",
              "indexing_variable": {
                  "x": {
                      "datatype": "Float64",
                      "dimensions": ["/x"],
                      "dimension_size": [20],
                      "attributes": {
                          "standard_name": "projection_x_coordinate",
                          "long_name": "x coordinate of projection",
                      },
                      "unit": "m",
                  }
              },
          },
          {
              "name": "y",
              "full_name": "/y",
              "size": 20,
              "type": "HORIZONTAL_Y",
              "indexing_variable": {
                  "y": {
                      "datatype": "Float64",
                      "dimensions": ["/y"],
                      "dimension_size": [20],
                      "attributes": {
                          "standard_name": "projection_y_coordinate",
                          "long_name": "y coordinate of projection",
                      },
                      "unit": "m",
                  }
              },
          },
      ],
      "arrays": {
          "x": {
              "datatype": "Float64",
              "dimensions": ["/x"],
              "dimension_size": [20],
              "attributes": {
                  "standard_name": "projection_x_coordinate",
                  "long_name": "x coordinate of projection",
              },
              "unit": "m",
          },
          "y": {
              "datatype": "Float64",
              "dimensions": ["/y"],
              "dimension_size": [20],
              "attributes": {
                  "standard_name": "projection_y_coordinate",
                  "long_name": "y coordinate of projection",
              },
              "unit": "m",
          },
          "Band1": {
              "datatype": "Byte",
              "dimensions": ["/y", "/x"],
              "dimension_size": [20, 20],
              "attributes": {
                  "long_name": "GDAL Band Number 1",
                  "valid_range": [0, 255],
              },
              "srs": {
                  "data_axis_to_srs_axis_mapping": [2, 1],
              },
          },
      },
      "structural_info": {"NC_FORMAT": "CLASSIC"},
  }


class GdalalgMdimInfoTest(gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if not utilities_util.has_driver("netCDF"):
      self.skipTest("netCDF driver not available")
    self.byte_nc = utilities_util.get_testdata_path(
        "../gdrivers/data/netcdf/byte.nc"
    )

  def test_mdim_info(self) -> None:
    info = _get_mdim_info_alg()
    self.assertTrue(info.ParseRunAndFinalize([self.byte_nc]))
    parsed = json.loads(info["output-string"])
    del parsed["arrays"]["Band1"]["srs"]["wkt"]
    self.assertEqual(parsed, _expected_byte_nc_summary())

  def test_mdim_info_all_options(self) -> None:
    info = _get_mdim_info_alg()
    self.assertTrue(
        info.ParseRunAndFinalize([
            self.byte_nc,
            "--detailed",
            "--stats",
            "--limit=5",
            "--array=/Band1",
            "--array-option=USE_DEFAULT_FILL_AS_NODATA=NO",
        ])
    )
    parsed = json.loads(info["output-string"])
    del parsed["dimensions"]
    del parsed["statistics"]["mean"]
    del parsed["statistics"]["stddev"]
    del parsed["srs"]["wkt"]
    self.assertEqual(
        parsed,
        {
            "type": "array",
            "name": "Band1",
            "datatype": "Byte",
            "dimension_size": [20, 20],
            "attributes": {
                "long_name": {
                    "datatype": "String",
                    "value": "GDAL Band Number 1",
                },
                "_Unsigned": {"datatype": "String", "value": "true"},
                "valid_range": {"datatype": "UInt16", "value": [0, 255]},
                "grid_mapping": {
                    "datatype": "String",
                    "value": "transverse_mercator",
                },
            },
            "srs": {
                "data_axis_to_srs_axis_mapping": [2, 1],
            },
            "values": [
                [181, 181, 156, "[...]", 99, 107],
                [173, 247, 255, "[...]", 107, 123],
                [156, 181, 140, "[...]", 140, 99],
                "[...]",
                [115, 132, 107, "[...]", 99, 156],
                [107, 123, 132, "[...]", 156, 148],
            ],
            "statistics": {"min": 74, "max": 255, "valid_sample_count": 400},
        },
    )

  def test_mdim_info_cli_binary(self) -> None:
    out, err, code = utilities_util.run_gdal_cli(["mdim", "info", self.byte_nc])
    self.assertEqual(code, 0, msg=err)
    parsed = json.loads(out)
    wkt = parsed["arrays"]["Band1"]["srs"].pop("wkt")
    srs = osr.SpatialReference(wkt)
    expected_srs = osr.SpatialReference()
    expected_srs.ImportFromEPSG(26711)
    self.assertIsSame(srs, expected_srs)
    self.assertEqual(parsed, _expected_byte_nc_summary())

  def test_mdim_info_completion_array(self) -> None:
    missing = os.path.join(os.path.dirname(self.byte_nc), "i_do_not_exist.nc")
    out, _, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "mdim", "info", missing, "--array"]
    )
    self.assertTrue(out.startswith("**"))

    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "mdim", "info", self.byte_nc, "--array"]
    )
    self.assertEqual(code, 0)
    self.assertEqual(out.split(" "), ["/x", "/y", "/Band1"])

  def test_mdim_info_completion_array_option(self) -> None:
    missing = os.path.join(os.path.dirname(self.byte_nc), "i_do_not_exist.nc")
    out, _, _ = utilities_util.run_gdal_cli(
        ["completion", "gdal", "mdim", "info", missing, "--array-option"]
    )
    self.assertTrue(out.startswith("**"))

    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "mdim", "info", self.byte_nc, "--array-option"]
    )
    self.assertEqual(code, 0)
    self.assertIn("USE_DEFAULT_FILL_AS_NODATA=", out.split(" "))


if __name__ == "__main__":
  absltest.main()
