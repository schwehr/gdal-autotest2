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

"""Tests for the GDALG driver.

Ported from upstream autotest/gdrivers/gdalg.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util


def setUpModule():
  gcore_util.SetupTestEnv()


class GdalgTest(gdaltest.TestCase):

  def test_driver_registered(self):
    drv = gdal.GetDriverByName("GDALG")
    self.assertIsNotNone(drv)

  def test_raster_pipeline_inline_and_file(self):
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      tif_path = os.path.join(tmpdir, "input.tif")
      drv = gdal.GetDriverByName("GTiff")
      self.assertIsNotNone(drv)
      ds = drv.Create(tif_path, 4, 4, 1, gdal.GDT_Byte)
      self.assertIsNotNone(ds)
      ds = None

      gdalg_path = os.path.join(tmpdir, "raster.gdalg.json")
      with open(gdalg_path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "type": "gdal_streamed_alg",
                "command_line": (
                    "gdal raster pipeline ! read input.tif ! set-type"
                    " --datatype UInt16"
                ),
            },
            f,
        )

      out_ds = gdal.OpenEx(gdalg_path, gdal.OF_RASTER)
      self.assertIsNotNone(out_ds)
      self.assertEqual(out_ds.GetDriver().ShortName, "GDALG")
      self.assertEqual(out_ds.RasterXSize, 4)
      self.assertEqual(out_ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
      self.assertEqual(out_ds.GetFileList(), [gdalg_path])
      out_ds = None

      # Vector open mode should not return a raster-only GDALG dataset.
      with gdal.quiet_errors():
        self.assertIsNone(gdal.OpenEx(gdalg_path, gdal.OF_VECTOR))

  def test_vector_pipeline_inline(self):
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      geojson_path = os.path.join(tmpdir, "pts.geojson")
      with open(geojson_path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "type": "FeatureCollection",
                "features": [
                    {
                        "type": "Feature",
                        "geometry": {
                            "type": "Point",
                            "coordinates": [1.0, 2.0],
                        },
                        "properties": {"id": 1},
                    },
                ],
            },
            f,
        )

      spec = json.dumps({
          "type": "gdal_streamed_alg",
          "command_line": f"gdal vector pipeline ! read {geojson_path}",
      })
      ds = gdal.OpenEx(spec, gdal.OF_VECTOR)
      self.assertIsNotNone(ds)
      self.assertEqual(ds.GetLayerCount(), 1)
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField("id"), 1)
      ds = None

  def test_errors(self):
    with gdal.quiet_errors():
      self.assertIsNone(
          gdal.OpenEx("{invalid", gdal.OF_ALL, allowed_drivers=["GDALG"])
      )
      self.assertIsNone(
          gdal.OpenEx(
              '{"type":"wrong","command_line":"gdal"}',
              gdal.OF_ALL,
              allowed_drivers=["GDALG"],
          )
      )


if __name__ == "__main__":
  absltest.main()
