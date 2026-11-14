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

"""Tests for `gdal raster pixel-info`.

Ported from upstream autotest/utilities/test_gdalalg_raster_pixel_info.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json

from absl.testing import absltest
from osgeo import gdal
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["pixel-info"]


class GdalalgRasterPixelInfoTest(gdaltest.TestCase):

  def test_missing_and_invalid_position(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["dataset"] = byte_tif
    with self.assertRaisesRegex(
        RuntimeError, "Argument 'position' must be specified"
    ):
      alg.Run()

    alg = _get_alg()
    with self.assertRaisesRegex(
        RuntimeError, "even number of values must be specified"
    ):
      alg["position"] = 5

  def test_byte_and_float64_json(self) -> None:
    for rel in ("../gcore/data/byte.tif", "../gcore/data/float64.tif"):
      alg = _get_alg()
      alg["dataset"] = utilities_util.get_testdata_path(rel)
      alg["position"] = [5, 10]
      self.assertTrue(alg.Run())
      parsed = json.loads(alg["output-string"])
      self.assertEqual(
          parsed,
          {
              "type": "FeatureCollection",
              "crs": {
                  "type": "name",
                  "properties": {"name": "urn:ogc:def:crs:EPSG::26711"},
              },
              "features": [{
                  "type": "Feature",
                  "properties": {
                      "input_coordinate": [5.0, 10.0],
                      "column": 5.0,
                      "line": 10.0,
                      "bands": [{
                          "band_number": 1,
                          "raw_value": 132,
                          "unscaled_value": 132,
                      }],
                  },
                  "geometry": {
                      "type": "Point",
                      "coordinates": [441020.0, 3750720.0],
                  },
              }],
          },
      )

  def test_complex_json(self) -> None:
    cfloat32_tif = utilities_util.get_testdata_path(
        "../gcore/data/cfloat32.tif"
    )
    alg = _get_alg()
    alg["dataset"] = cfloat32_tif
    alg["position"] = [5, 10]
    self.assertTrue(alg.Run())
    parsed = json.loads(alg["output-string"])
    self.assertEqual(
        parsed,
        {
            "type": "FeatureCollection",
            "crs": {
                "type": "name",
                "properties": {"name": "urn:ogc:def:crs:EPSG::26711"},
            },
            "features": [{
                "type": "Feature",
                "properties": {
                    "input_coordinate": [5.0, 10.0],
                    "column": 5.0,
                    "line": 10.0,
                    "bands": [{
                        "band_number": 1,
                        "value": {"real": 132.0, "imaginary": 0.0},
                    }],
                },
                "geometry": {
                    "type": "Point",
                    "coordinates": [441020.0, 3750720.0],
                },
            }],
        },
    )

  def test_csv_outputs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    cfloat32_tif = utilities_util.get_testdata_path(
        "../gcore/data/cfloat32.tif"
    )

    alg = _get_alg()
    alg["dataset"] = byte_tif
    alg["position"] = [5, 10]
    alg["format"] = "csv"
    self.assertTrue(alg.Run())
    self.assertEqual(
        alg["output-string"],
        "input_x,input_y,extra_input,column,line,"
        "band_1_raw_value,band_1_unscaled_value\n"
        '5,10,"",5,10,132,132\n',
    )

    alg = _get_alg()
    alg["dataset"] = byte_tif
    alg["position"] = [-5, 10]
    alg["format"] = "csv"
    self.assertTrue(alg.Run())
    self.assertEqual(
        alg["output-string"],
        "input_x,input_y,extra_input,column,line,"
        "band_1_raw_value,band_1_unscaled_value\n"
        '-5,10,"",-5,10,,\n',
    )

    alg = _get_alg()
    alg["dataset"] = cfloat32_tif
    alg["position"] = [5, 10]
    alg["format"] = "csv"
    self.assertTrue(alg.Run())
    self.assertEqual(
        alg["output-string"],
        "input_x,input_y,extra_input,column,line,"
        "band_1_real_value,band_1_imaginary_value\n"
        '5,10,"",5,10,132,0\n',
    )

    alg = _get_alg()
    alg["dataset"] = cfloat32_tif
    alg["position"] = [-5, 10]
    alg["format"] = "csv"
    self.assertTrue(alg.Run())
    self.assertEqual(
        alg["output-string"],
        "input_x,input_y,extra_input,column,line,"
        "band_1_real_value,band_1_imaginary_value\n"
        '-5,10,"",-5,10,,\n',
    )

  def test_overviews(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["dataset"] = byte_tif
    alg["overview"] = 0
    with self.assertRaisesRegex(
        RuntimeError,
        "Source dataset has no overviews. Argument 'overview' must not be"
        " specified",
    ):
      alg.Run()

    src_ds = gdal.Translate("", byte_tif, format="MEM")
    src_ds.BuildOverviews("NEAR", [2])

    alg = _get_alg()
    alg["dataset"] = src_ds
    alg["overview"] = 1
    with self.assertRaisesRegex(
        RuntimeError,
        "Source dataset has only 1 overview level. 'overview' value must be"
        " strictly lower than this number",
    ):
      alg.Run()

    alg = _get_alg()
    alg["dataset"] = src_ds
    alg["overview"] = 0
    alg["position"] = [5, 10]
    self.assertTrue(alg.Run())
    parsed = json.loads(alg["output-string"])
    self.assertEqual(
        parsed,
        {
            "type": "FeatureCollection",
            "crs": {
                "type": "name",
                "properties": {"name": "urn:ogc:def:crs:EPSG::26711"},
            },
            "features": [{
                "type": "Feature",
                "properties": {
                    "input_coordinate": [5.0, 10.0],
                    "column": 5.0,
                    "line": 10.0,
                    "bands": [{
                        "band_number": 1,
                        "raw_value": 99,
                        "unscaled_value": 99,
                    }],
                },
                "geometry": {
                    "type": "Point",
                    "coordinates": [441020.0, 3750720.0],
                },
            }],
        },
    )

  def test_unscaled(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.GetRasterBand(1).Fill(1)
    src_ds.GetRasterBand(1).SetOffset(2)
    src_ds.GetRasterBand(1).SetScale(3)

    alg = _get_alg()
    alg["dataset"] = src_ds
    alg["position"] = [0, 0]
    self.assertTrue(alg.Run())
    parsed = json.loads(alg["output-string"])
    self.assertEqual(
        parsed,
        {
            "type": "FeatureCollection",
            "features": [{
                "type": "Feature",
                "properties": {
                    "input_coordinate": [0.0, 0.0],
                    "column": 0.0,
                    "line": 0.0,
                    "bands": [{
                        "band_number": 1,
                        "raw_value": 1,
                        "unscaled_value": 5.0,
                    }],
                },
                "geometry": None,
            }],
        },
    )

    src_f32 = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_Float32)
    src_f32.GetRasterBand(1).Fill(1.5)
    src_f32.GetRasterBand(1).SetOffset(2)
    src_f32.GetRasterBand(1).SetScale(3)

    alg = _get_alg()
    alg["dataset"] = src_f32
    alg["position"] = [0, 0]
    alg["format"] = "csv"
    self.assertTrue(alg.Run())
    self.assertEqual(
        alg["output-string"],
        "input_x,input_y,extra_input,column,line,"
        "band_1_raw_value,band_1_unscaled_value\n"
        '0,0,"",0,0,1.5,6.5\n',
    )

  def test_missing_crs_and_geotransform_errors(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src_ds = mem_drv.Create("", 1, 1)
    alg = _get_alg()
    alg["dataset"] = src_ds
    alg["position"] = [0, 0]
    alg["position-crs"] = "dataset"
    alg["format"] = "csv"
    with self.assertRaisesRegex(
        RuntimeError,
        "Dataset has no CRS. Only 'position-crs' = 'pixel' is supported",
    ):
      alg.Run()

    src_ds = mem_drv.Create("", 1, 1)
    srs = osr.SpatialReference("WGS84")
    src_ds.SetSpatialRef(srs)
    alg = _get_alg()
    alg["dataset"] = src_ds
    alg["position"] = [0, 0]
    alg["position-crs"] = "WGS84"
    alg["format"] = "csv"
    with self.assertRaisesRegex(RuntimeError, "Cannot get geotransform"):
      alg.Run()

    src_ds.SetGeoTransform([0, 0, 0, 0, 0, 0])
    alg = _get_alg()
    alg["dataset"] = src_ds
    alg["position"] = [0, 0]
    alg["position-crs"] = "WGS84"
    alg["format"] = "csv"
    with self.assertRaisesRegex(RuntimeError, "Cannot invert geotransform"):
      alg.Run()

  def test_position_crs_and_non_epsg(self) -> None:
    mem_drv = gdal.GetDriverByName("MEM")
    src_ds = mem_drv.Create("", 1, 1)
    srs = osr.SpatialReference("WGS84")
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    src_ds.SetSpatialRef(srs)
    src_ds.SetGeoTransform([10, 1, 0, 20, 0, 1])

    for pos_crs in ("dataset", srs):
      alg = _get_alg()
      alg["dataset"] = src_ds
      alg["position"] = [10.5, 20.5]
      alg["position-crs"] = pos_crs
      self.assertTrue(alg.Run())
      parsed = json.loads(alg["output-string"])
      self.assertEqual(
          parsed,
          {
              "type": "FeatureCollection",
              "crs": {
                  "type": "name",
                  "properties": {"name": "urn:ogc:def:crs:OGC:1.3:CRS84"},
              },
              "features": [{
                  "type": "Feature",
                  "properties": {
                      "input_coordinate": [10.5, 20.5],
                      "column": 0.5,
                      "line": 0.5,
                      "bands": [{
                          "band_number": 1,
                          "raw_value": 0,
                          "unscaled_value": 0,
                      }],
                  },
                  "geometry": {"type": "Point", "coordinates": [10.5, 20.5]},
              }],
          },
      )

    src_utm = mem_drv.Create("", 1, 1)
    srs_utm = osr.SpatialReference("+proj=utm +zone=17")
    srs_utm.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    src_utm.SetSpatialRef(srs_utm)
    src_utm.SetGeoTransform([500000, 1, 0, 0, 0, 1])

    alg = _get_alg()
    alg["dataset"] = src_utm
    alg["position"] = [0, 0]
    self.assertTrue(alg.Run())
    parsed = json.loads(alg["output-string"])
    self.assertEqual(
        parsed,
        {
            "type": "FeatureCollection",
            "crs": {
                "type": "name",
                "properties": {"name": "urn:ogc:def:crs:OGC:1.3:CRS84"},
            },
            "features": [{
                "type": "Feature",
                "properties": {
                    "input_coordinate": [0.0, 0.0],
                    "column": 0.0,
                    "line": 0.0,
                    "bands": [{
                        "band_number": 1,
                        "raw_value": 0,
                        "unscaled_value": 0,
                    }],
                },
                "geometry": {"type": "Point", "coordinates": [-81.0, 0.0]},
            }],
        },
    )

  def test_files(self) -> None:
    byte_vrt = utilities_util.get_testdata_path("../gcore/data/byte.vrt")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["dataset"] = byte_vrt
    alg["position"] = [5, 10]
    self.assertTrue(alg.Run())
    parsed = json.loads(
        alg["output-string"].replace("\\\\", "/").replace("\\/", "/")
    )
    self.assertEqual(
        parsed,
        {
            "type": "FeatureCollection",
            "crs": {
                "type": "name",
                "properties": {"name": "urn:ogc:def:crs:EPSG::26711"},
            },
            "features": [{
                "type": "Feature",
                "properties": {
                    "input_coordinate": [5.0, 10.0],
                    "column": 5.0,
                    "line": 10.0,
                    "bands": [{
                        "band_number": 1,
                        "raw_value": 132,
                        "unscaled_value": 132,
                        "files": [byte_tif.replace("\\", "/")],
                    }],
                },
                "geometry": {
                    "type": "Point",
                    "coordinates": [441020.0, 3750720.0],
                },
            }],
        },
    )

  def test_from_command_line(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pixel-info", byte_tif],
        stdin_text="5.5 10.5 foo bar",
    )
    self.assertEqual(code, 0)
    self.assertEqual(
        json.loads(out),
        {
            "type": "FeatureCollection",
            "crs": {
                "type": "name",
                "properties": {"name": "urn:ogc:def:crs:EPSG::26711"},
            },
            "features": [{
                "type": "Feature",
                "properties": {
                    "input_coordinate": [5.5, 10.5],
                    "extra_content": "foo bar",
                    "column": 5.5,
                    "line": 10.5,
                    "bands": [{
                        "band_number": 1,
                        "raw_value": 132,
                        "unscaled_value": 132,
                    }],
                },
                "geometry": {
                    "type": "Point",
                    "coordinates": [441050.0, 3750690.0],
                },
            }],
        },
    )

    out_csv, _, code = utilities_util.run_gdal_cli(
        ["raster", "pixel-info", "--of=csv", byte_tif],
        stdin_text="5.5 10.5 foo bar",
    )
    self.assertEqual(code, 0)
    self.assertEqual(
        out_csv.replace("\r\n", "\n"),
        "input_x,input_y,extra_input,column,line,"
        "band_1_raw_value,band_1_unscaled_value\n"
        '5.5,10.5,"foo bar",5.5,10.5,132,132\n',
    )


if __name__ == "__main__":
  absltest.main()
