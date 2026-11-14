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

"""Tests for `gdal mdim convert`.

Ported from upstream autotest/utilities/test_gdalalg_mdim_convert.py.

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


def _get_mdim_convert_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["mdim"]["convert"]


class GdalalgMdimConvertTest(gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if not utilities_util.has_driver("VRT"):
      self.skipTest("VRT driver not available")

  def test_basic(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    with utilities_util.vsimem_dir("mdim_convert_basic") as tmp:
      out_vrt = f"{tmp}/out.vrt"
      alg = _get_mdim_convert_alg()
      alg["strict"] = True
      alg["array-option"] = ["NOT_A_REAL=OPTION"]
      self.assertTrue(alg.ParseRunAndFinalize([mdim_vrt, out_vrt]))
      self.assertEqual(gdal.MultiDimInfo(out_vrt), gdal.MultiDimInfo(mdim_vrt))

  def test_overwrite(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    with utilities_util.vsimem_dir("mdim_convert_overwrite") as tmp:
      out_vrt = f"{tmp}/out.vrt"
      self.assertTrue(
          _get_mdim_convert_alg().ParseRunAndFinalize([mdim_vrt, out_vrt])
      )

      with self.assertRaisesRegex(RuntimeError, "already exists"):
        _get_mdim_convert_alg().ParseRunAndFinalize([mdim_vrt, out_vrt])

      alg = _get_mdim_convert_alg()
      alg["overwrite"] = True
      self.assertTrue(alg.ParseRunAndFinalize([mdim_vrt, out_vrt]))
      self.assertEqual(gdal.MultiDimInfo(out_vrt), gdal.MultiDimInfo(mdim_vrt))

  def test_to_mem(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    alg = _get_mdim_convert_alg()
    alg["input"] = mdim_vrt
    alg["output-format"] = "MEM"
    alg["output"] = ""
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    if out_ds is None:
      self.fail("Expected output dataset")
    rg = out_ds.GetRootGroup()
    if rg is None:
      self.fail("Expected root group")
    ar = rg.OpenMDArray("time_increasing")
    if ar is None:
      self.fail("Expected time_increasing array")
    self.assertEqual(
        ar.Read(), ["2010-01-01", "2011-01-01", "2012-01-01", "2013-01-01"]
    )

  def test_multidim_to_classic(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    with utilities_util.vsimem_dir("mdim_convert_classic") as tmp:
      out_tif = f"{tmp}/out.tif"
      alg = _get_mdim_convert_alg()
      alg["input"] = mdim_vrt
      alg["output"] = out_tif
      alg["array"] = ["/my_subgroup/array_in_subgroup"]
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

  def test_group(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    with utilities_util.vsimem_dir("mdim_convert_group") as tmp:
      out_vrt = f"{tmp}/out.vrt"
      alg = _get_mdim_convert_alg()
      alg["input"] = mdim_vrt
      alg["output"] = out_vrt
      alg["group"] = ["my_subgroup"]
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      got_data = gcore_util.vsi_read_file_as_string(out_vrt, 10000)

    expected = (
        "\n".join([
            "<VRTDataset>",
            '  <Group name="/">',
            (
                '    <Dimension name="latitude" type="HORIZONTAL_Y"'
                ' direction="NORTH" size="10" indexingVariable="latitude" />'
            ),
            (
                '    <Dimension name="longitude" type="HORIZONTAL_X"'
                ' direction="EAST" size="10" indexingVariable="longitude" />'
            ),
            '    <Array name="latitude">',
            "      <DataType>Float32</DataType>",
            '      <DimensionRef ref="latitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/latitude</SourceArray>",
            '        <SourceSlab offset="0" count="10" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="longitude">',
            "      <DataType>Float32</DataType>",
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/longitude</SourceArray>",
            '        <SourceSlab offset="0" count="10" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="array_in_subgroup">',
            "      <DataType>Int32</DataType>",
            '      <DimensionRef ref="latitude" />',
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/my_subgroup/array_in_subgroup</SourceArray>",
            '        <SourceSlab offset="0,0" count="10,10" step="1,1" />',
            '        <DestSlab offset="0,0" />',
            "      </Source>",
            "    </Array>",
            "  </Group>",
            "</VRTDataset>",
        ])
        + "\n"
    )
    self.assertEqual(got_data, expected)

  def test_subset(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    with utilities_util.vsimem_dir("mdim_convert_subset") as tmp:
      out_vrt = f"{tmp}/out.vrt"
      alg = _get_mdim_convert_alg()
      alg["input"] = mdim_vrt
      alg["output"] = out_vrt
      alg["subset"] = ["latitude(70,87.5)", 'time_increasing("2012-01-01")']
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      got_data = gcore_util.vsi_read_file_as_string(out_vrt, 10000)

    expected = (
        "\n".join([
            "<VRTDataset>",
            '  <Group name="/">',
            (
                '    <Dimension name="latitude" type="HORIZONTAL_Y"'
                ' direction="NORTH" size="8" indexingVariable="latitude" />'
            ),
            (
                '    <Dimension name="longitude" type="HORIZONTAL_X"'
                ' direction="EAST" size="10" indexingVariable="longitude" />'
            ),
            (
                '    <Dimension name="time_decreasing" type="TEMPORAL" size="4"'
                ' indexingVariable="time_decreasing" />'
            ),
            '    <Array name="latitude">',
            "      <DataType>Float32</DataType>",
            '      <DimensionRef ref="latitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/latitude</SourceArray>",
            "        <SourceView>[1:9:1]</SourceView>",
            '        <SourceSlab offset="0" count="8" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="longitude">',
            "      <DataType>Float32</DataType>",
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/longitude</SourceArray>",
            '        <SourceSlab offset="0" count="10" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="my_variable_with_time_increasing">',
            "      <DataType>Int32</DataType>",
            '      <DimensionRef ref="latitude" />',
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            (
                "        <SourceArray>/my_variable_with_time_increasing"
                "</SourceArray>"
            ),
            "        <SourceView>[2,1:9:1,:]</SourceView>",
            '        <SourceSlab offset="0,0" count="8,10" step="1,1" />',
            '        <DestSlab offset="0,0" />',
            "      </Source>",
            '      <Attribute name="DIM_time_increasing_INDEX">',
            "        <DataType>Int32</DataType>",
            "        <Value>2</Value>",
            "      </Attribute>",
            '      <Attribute name="DIM_time_increasing_VALUE">',
            "        <DataType>String</DataType>",
            "        <Value>2012-01-01</Value>",
            "      </Attribute>",
            "    </Array>",
            '    <Array name="time_increasing">',
            "      <DataType>String</DataType>",
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/time_increasing</SourceArray>",
            "        <SourceView>[2]</SourceView>",
            "      </Source>",
            '      <Attribute name="DIM_time_increasing_INDEX">',
            "        <DataType>Int32</DataType>",
            "        <Value>2</Value>",
            "      </Attribute>",
            '      <Attribute name="DIM_time_increasing_VALUE">',
            "        <DataType>String</DataType>",
            "        <Value>2012-01-01</Value>",
            "      </Attribute>",
            "    </Array>",
            '    <Array name="my_variable_with_time_decreasing">',
            "      <DataType>Int32</DataType>",
            '      <DimensionRef ref="time_decreasing" />',
            '      <DimensionRef ref="latitude" />',
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            (
                "        <SourceArray>/my_variable_with_time_decreasing"
                "</SourceArray>"
            ),
            "        <SourceView>[:,1:9:1,:]</SourceView>",
            '        <SourceSlab offset="0,0,0" count="4,8,10" step="1,1,1" />',
            '        <DestSlab offset="0,0,0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="time_decreasing">',
            "      <DataType>String</DataType>",
            '      <DimensionRef ref="time_decreasing" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/time_decreasing</SourceArray>",
            '        <SourceSlab offset="0" count="4" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Group name="my_subgroup">',
            '      <Array name="array_in_subgroup">',
            "        <DataType>Int32</DataType>",
            '        <DimensionRef ref="/latitude" />',
            '        <DimensionRef ref="/longitude" />',
            "        <Source>",
            f"          <SourceFilename>{mdim_vrt}</SourceFilename>",
            (
                "          <SourceArray>/my_subgroup/array_in_subgroup"
                "</SourceArray>"
            ),
            "          <SourceView>[1:9:1,:]</SourceView>",
            '          <SourceSlab offset="0,0" count="8,10" step="1,1" />',
            '          <DestSlab offset="0,0" />',
            "        </Source>",
            "      </Array>",
            "    </Group>",
            '    <Group name="other_subgroup">',
            '      <Attribute name="foo">',
            "        <DataType>String</DataType>",
            "        <Value>bar</Value>",
            "      </Attribute>",
            "    </Group>",
            "  </Group>",
            "</VRTDataset>",
        ])
        + "\n"
    )
    self.assertEqual(got_data, expected)

  def test_scaleaxes(self) -> None:
    mdim_vrt = utilities_util.get_testdata_path("data/mdim.vrt")
    with utilities_util.vsimem_dir("mdim_convert_scaleaxes") as tmp:
      out_vrt = f"{tmp}/out.vrt"
      alg = _get_mdim_convert_alg()
      alg["input"] = mdim_vrt
      alg["output"] = out_vrt
      alg["array"] = ["my_variable_with_time_increasing"]
      alg["scale-axes"] = ["longitude(2)"]
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      got_data = gcore_util.vsi_read_file_as_string(out_vrt, 10000)

    expected = (
        "\n".join([
            "<VRTDataset>",
            '  <Group name="/">',
            (
                '    <Dimension name="latitude" type="HORIZONTAL_Y"'
                ' direction="NORTH" size="10" indexingVariable="latitude" />'
            ),
            (
                '    <Dimension name="longitude" type="HORIZONTAL_X"'
                ' direction="EAST" size="5" indexingVariable="longitude" />'
            ),
            (
                '    <Dimension name="time_increasing" type="TEMPORAL" size="4"'
                ' indexingVariable="time_increasing" />'
            ),
            '    <Array name="time_increasing">',
            "      <DataType>String</DataType>",
            '      <DimensionRef ref="time_increasing" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/time_increasing</SourceArray>",
            '        <SourceSlab offset="0" count="4" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="latitude">',
            "      <DataType>Float32</DataType>",
            '      <DimensionRef ref="latitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/latitude</SourceArray>",
            '        <SourceSlab offset="0" count="10" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="longitude">',
            "      <DataType>Float32</DataType>",
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            "        <SourceArray>/longitude</SourceArray>",
            "        <SourceView>[0:10:2]</SourceView>",
            '        <SourceSlab offset="0" count="5" step="1" />',
            '        <DestSlab offset="0" />',
            "      </Source>",
            "    </Array>",
            '    <Array name="my_variable_with_time_increasing">',
            "      <DataType>Int32</DataType>",
            '      <DimensionRef ref="time_increasing" />',
            '      <DimensionRef ref="latitude" />',
            '      <DimensionRef ref="longitude" />',
            "      <Source>",
            f"        <SourceFilename>{mdim_vrt}</SourceFilename>",
            (
                "        <SourceArray>/my_variable_with_time_increasing"
                "</SourceArray>"
            ),
            "        <SourceView>[:,:,0:10:2]</SourceView>",
            '        <SourceSlab offset="0,0,0" count="4,10,5" step="1,1,1" />',
            '        <DestSlab offset="0,0,0" />',
            "      </Source>",
            "    </Array>",
            "  </Group>",
            "</VRTDataset>",
        ])
        + "\n"
    )
    self.assertEqual(got_data, expected)

  def test_creation_option(self) -> None:
    if not utilities_util.has_driver("netCDF"):
      self.skipTest("netCDF driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_nc = os.path.join(tmpdir, "out.nc")
      alg = _get_mdim_convert_alg()
      alg["input"] = byte_tif
      alg["output"] = out_nc
      alg["creation-option"] = ["COMPRESS=DEFLATE"]
      with gdal.quiet_errors():
        self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      info = gdal.MultiDimInfo(out_nc)
      self.assertEqual(
          info["arrays"]["Band1"]["structural_info"], {"COMPRESS": "DEFLATE"}
      )

  def test_completion_array(self) -> None:
    if not utilities_util.has_driver("netCDF"):
      self.skipTest("netCDF driver not available")
    byte_nc = utilities_util.get_testdata_path(
        "../gdrivers/data/netcdf/byte.nc"
    )
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "mdim", "convert", byte_nc, "--array"]
    )
    self.assertEqual(code, 0)
    self.assertEqual(out.split(" "), ["/x", "/y", "/Band1"])

  def test_completion_array_option(self) -> None:
    if not utilities_util.has_driver("netCDF"):
      self.skipTest("netCDF driver not available")
    byte_nc = utilities_util.get_testdata_path(
        "../gdrivers/data/netcdf/byte.nc"
    )
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "mdim", "convert", byte_nc, "--array-option"]
    )
    self.assertEqual(code, 0)
    self.assertIn("USE_DEFAULT_FILL_AS_NODATA=", out.split(" "))

  def test_valid_transpose_axis(self) -> None:
    if not utilities_util.has_driver("ZARR"):
      self.skipTest("ZARR driver not available")
    zarr_src = utilities_util.get_testdata_path(
        "../gdrivers/data/zarr/array_dimensions.zarr"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_zarr = os.path.join(tmpdir, "out.zarr")
      alg = _get_mdim_convert_alg()
      alg["input"] = zarr_src
      alg["output"] = out_zarr
      alg["output-format"] = "ZARR"
      alg["array"] = "name=var,transpose=[a,1]"
      with self.assertRaisesRegex(
          RuntimeError, r"Invalid value for axis in transpose: a"
      ):
        alg.Run()


if __name__ == "__main__":
  absltest.main()
