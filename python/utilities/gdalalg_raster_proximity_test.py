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

"""Tests for `gdal raster proximity`.

Ported from upstream autotest/utilities/test_gdalalg_raster_proximity.py.

Upstream copyright:
  Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Sequence
from typing import Any

from absl.testing import absltest
from absl.testing import parameterized
import numpy as np
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_proximity_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["proximity"]


def _create_gtiff_from_array(
    filename: str,
    data_array: np.ndarray,
    gt: Sequence[float] = (0, 1, 0, 0, 0, -1),
    srs_wkt: str | None = None,
    nodata_val: float | None = None,
    dtype: int = gdal.GDT_Byte,
) -> None:
  driver = gdal.GetDriverByName("GTiff")
  rows, cols = data_array.shape
  dataset = driver.Create(filename, cols, rows, 2, dtype)
  dataset.SetGeoTransform(gt)
  if srs_wkt:
    dataset.SetProjection(srs_wkt)

  band1 = dataset.GetRasterBand(1)
  if nodata_val is not None:
    band1.SetNoDataValue(nodata_val)
  band1.WriteArray(data_array)

  band2 = dataset.GetRasterBand(2)
  band2.WriteArray(np.fliplr(data_array))
  if nodata_val is not None:
    band2.SetNoDataValue(nodata_val)

  dataset.Close()


class GdalalgRasterProximityTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      (
          {
              "datatype": "Byte",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 0,
          },
          np.array([[0, 0, 2], [0, 1, 1], [2, 1, 0]], dtype=np.uint8),
      ),
      (
          {
              "datatype": "Float32",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 0,
          },
          np.array(
              [[0.0, 0.0, 2.0], [0.0, 1.4142135, 1.0], [2.0, 1.0, 0.0]],
              dtype=np.float32,
          ),
      ),
      (
          {
              "datatype": "Byte",
              "target-values": [1, 3],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 0,
          },
          np.array([[0, 1, 2], [1, 1, 1], [2, 1, 0]], dtype=np.uint8),
      ),
      (
          {
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 255,
          },
          np.array(
              [[255, 255, 2], [255, 1.4142135, 1], [2, 1, 0]],
              dtype=np.float32,
          ),
      ),
      (
          {
              "datatype": "Float32",
              "target-values": [1],
              "distance-units": "GEO",
              "max-distance": 2,
              "nodata": 0,
          },
          np.array([[0, 0, 2], [0, 1.4142135, 1], [2, 1, 0]], dtype=np.float32),
      ),
      (
          {
              "datatype": "Byte",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 0,
              "creation-option": {"TILED": "YES"},
          },
          np.array([[0, 0, 2], [0, 1, 1], [2, 1, 0]], dtype=np.uint8),
      ),
      (
          {
              "datatype": "Byte",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 255,
              "fixed-value": 128,
          },
          np.array(
              [[255, 255, 128], [255, 128, 128], [128, 128, 0]], dtype=np.uint8
          ),
      ),
      (
          {
              "datatype": "Byte",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "fixed-value": 128,
          },
          np.array(
              [[255, 255, 128], [255, 128, 128], [128, 128, 0]], dtype=np.uint8
          ),
      ),
      (
          {
              "datatype": "Int8",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "fixed-value": 1,
          },
          np.array([[127, 127, 1], [127, 1, 1], [1, 1, 0]], dtype=np.int8),
      ),
      (
          {
              "datatype": "Float32",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "fixed-value": 128,
          },
          np.array(
              [[65535, 65535, 128], [65535, 128, 128], [128, 128, 0]],
              dtype=np.float32,
          ),
      ),
      (
          {
              "band": 2,
              "datatype": "Byte",
              "target-values": [1],
              "distance-units": "PIXEL",
              "max-distance": 2,
              "nodata": 0,
          },
          np.array([[2, 0, 0], [1, 1, 0], [0, 1, 2]], dtype=np.uint8),
      ),
  )
  def test_options(
      self, options: dict[str, Any], expected_output_data: np.ndarray
  ) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    input_data = np.array([[3, 0, 0], [0, 0, 0], [0, 0, 1]], dtype=np.uint8)
    with utilities_util.vsimem_dir("proximity_opts") as tmp:
      src_path = f"{tmp}/prox_in.tif"
      dst_path = f"{tmp}/prox_out.tif"
      _create_gtiff_from_array(
          src_path, input_data, gt=(10, 1, 0, 45, 0, -1), srs_wkt="EPSG:4326"
      )
      src_ds = gdal.Open(src_path)

      alg = _get_proximity_alg()
      alg["input"] = src_path
      alg["output"] = dst_path
      for k, v in options.items():
        alg[k] = v

      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      expected_dtype = (
          gdal.GetDataTypeByName(options["datatype"])
          if "datatype" in options
          else gdal.GDT_Float32
      )

      with gdal.Open(dst_path) as out_ds:
        self.assertIsNotNone(out_ds)
        band_out = out_ds.GetRasterBand(1)
        self.assertEqual(band_out.DataType, expected_dtype)
        output_data = band_out.ReadAsArray()
        self.assertTrue(
            np.allclose(output_data, expected_output_data, atol=1e-6)
        )
        self.assertEqual(out_ds.GetGeoTransform(), src_ds.GetGeoTransform())
        self.assertEqual(out_ds.GetProjection(), src_ds.GetProjection())
        if options.get("creation-option", {}).get("TILED") == "YES":
          self.assertEqual(band_out.GetBlockSize(), [256, 256])

  def test_overwrite(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    input_data = np.array([[0, 1, 0]], dtype=np.uint8)
    with utilities_util.vsimem_dir("proximity_ow") as tmp:
      src_path = f"{tmp}/prox_in_ow.tif"
      dst_path = f"{tmp}/prox_out_ow.tif"
      _create_gtiff_from_array(src_path, input_data)

      last_pct = [0.0]

      def on_progress(pct: float, msg: str, user_data: Any) -> bool:
        del msg, user_data
        self.assertGreaterEqual(pct, last_pct[0])
        last_pct[0] = pct
        return True

      alg = _get_proximity_alg()
      alg["input"] = src_path
      alg["output"] = dst_path
      alg["target-values"] = [1]
      self.assertTrue(alg.Run(on_progress))
      self.assertTrue(alg.Finalize())
      self.assertEqual(last_pct[0], 1.0)

      alg2 = _get_proximity_alg()
      alg2["input"] = src_path
      alg2["output"] = dst_path
      alg2["target-values"] = [1]
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg2.Run()

      alg3 = _get_proximity_alg()
      alg3["input"] = src_path
      alg3["output"] = dst_path
      alg3["target-values"] = [1]
      alg3["overwrite"] = True
      self.assertTrue(alg3.Run())
      self.assertTrue(alg3.Finalize())

  def test_respect_input_nodata(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    input_data = np.array([[3, 0, 255], [0, 0, 0], [0, 0, 1]], dtype=np.uint8)
    with utilities_util.vsimem_dir("proximity_nodata") as tmp:
      src_path = f"{tmp}/prox_in_nodata.tif"
      dst_path = f"{tmp}/prox_out_nodata.tif"
      _create_gtiff_from_array(src_path, input_data, nodata_val=255)

      expected = np.array(
          [[128, 128, 128], [128, 1, 1], [2, 1, 0]], dtype=np.uint8
      )

      alg = _get_proximity_alg()
      alg["input"] = src_path
      alg["output"] = dst_path
      alg["target-values"] = [1]
      alg["distance-units"] = "PIXEL"
      alg["max-distance"] = 2
      alg["datatype"] = "Byte"
      alg["nodata"] = 128
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(dst_path) as out_ds:
        output_data = out_ds.GetRasterBand(1).ReadAsArray()
        self.assertTrue(np.allclose(output_data, expected, atol=1e-6))

  def test_in_pipeline_invalid_band(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with self.assertRaisesRegex(
        RuntimeError,
        "proximity: Value of 'band' should be greater or equal than 1 and"
        " less or equal than 1",
    ):
      gdal.Run(
          "raster",
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! proximity --band 2 ! write --of=stream"
              " streamed_dataset"
          ),
      )

  def test_cannot_create_temp_file(self) -> None:
    if not utilities_util.has_driver("GTiff"):
      self.skipTest("GTiff driver not available")
    input_data = np.array([[3, 0, 255], [0, 0, 0], [0, 0, 1]], dtype=np.uint8)
    with utilities_util.vsimem_dir("proximity_tmp_err") as tmp:
      src_path = f"{tmp}/prox_in_nodata.tif"
      dst_path = f"{tmp}/prox_out_nodata.tif"
      _create_gtiff_from_array(src_path, input_data, nodata_val=255)

      alg = _get_proximity_alg()
      alg["input"] = src_path
      alg["output"] = dst_path
      alg["target-values"] = [1]
      alg["distance-units"] = "PIXEL"
      alg["max-distance"] = 2
      alg["datatype"] = "Byte"
      alg["nodata"] = 128
      with gdal.config_options({
          "GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET": "YES",
          "CPL_TMPDIR": "/i_do/not/exist",
      }):
        with self.assertRaises(RuntimeError):
          alg.Run()


if __name__ == "__main__":
  absltest.main()
