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

"""Tests for `gdal vector simplify`.

Ported from upstream autotest/utilities/test_gdalalg_vector_simplify.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["simplify"]


class GdalalgVectorSimplifyTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if ogr.GetGEOSVersionMajor() == 0:
      self.skipTest("GEOS not available")

  def test_simplify(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    src_lyr = src_ds.CreateLayer("the_layer", srs=srs)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("LINESTRING(0 0,1 1,2 0)"))
    src_lyr.CreateFeature(feat)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    src_lyr.CreateFeature(feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    alg["tolerance"] = 2
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_f = out_lyr.GetNextFeature()
    self.assertEqual(
        out_f.GetGeometryRef().ExportToWkt(), "LINESTRING (0 0,2 0)"
    )
    self.assertEqual(
        out_f.GetGeometryRef().GetSpatialReference().GetAuthorityCode(None),
        "32631",
    )
    out_f = out_lyr.GetNextFeature()
    self.assertIsNone(out_f.GetGeometryRef())
    self.assertEqual(out_lyr.GetFeatureCount(), 2)

    out_lyr.SetAttributeFilter("0 = 1")
    self.assertEqual(out_lyr.GetFeatureCount(), 0)
    out_lyr.SetAttributeFilter(None)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
    self.assertEqual(out_lyr.TestCapability(ogr.OLCRandomWrite), 0)
    self.assertEqual(out_lyr.GetExtent(), (0, 2, 0, 1))
    self.assertEqual(out_lyr.GetFeature(0).GetFID(), 0)
    self.assertIsNone(out_lyr.GetFeature(-1))

  @parameterized.parameters(-1, float("nan"))
  def test_error(self, tol: float) -> None:
    alg = _get_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Value of argument 'tolerance' is .*, but should be >= 0",
    ):
      alg["tolerance"] = tol


if __name__ == "__main__":
  absltest.main()
