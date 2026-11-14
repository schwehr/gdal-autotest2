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

"""Tests for `gdal vector buffer`.

Ported from upstream autotest/utilities/test_gdalalg_vector_buffer.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from typing import Any

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
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["buffer"]


def _has_geos(min_major: int = 0, min_minor: int = 0) -> bool:
  major = ogr.GetGEOSVersionMajor()
  minor = ogr.GetGEOSVersionMinor()
  return major > 0 and (major, minor) >= (min_major, min_minor)


class GdalalgVectorBufferTest(gdaltest.TestCase, parameterized.TestCase):

  def _assert_geometry_matches(
      self,
      actual: ogr.Geometry,
      expected: ogr.Geometry,
      max_error: float = 1e-4,
  ) -> None:
    if (
        _has_geos()
        and not actual.Is3D()
        and not actual.IsMeasured()
        and actual.Within(expected)
        and expected.Within(actual)
    ):
      return

    self.assertEqual(actual.GetGeometryName(), expected.GetGeometryName())
    self.assertEqual(actual.GetGeometryCount(), expected.GetGeometryCount())
    if actual.GetGeometryCount() > 0:
      for i in range(actual.GetGeometryCount()):
        self._assert_geometry_matches(
            actual.GetGeometryRef(i), expected.GetGeometryRef(i), max_error
        )
    else:
      self.assertEqual(actual.GetPointCount(), expected.GetPointCount())
      for i in range(actual.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(i), expected.GetX(i), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetY(i), expected.GetY(i), delta=max_error
        )

  @parameterized.named_parameters(
      (
          "default",
          "POINT (0 0)",
          {"distance": 1},
          (
              "POLYGON ((1 0,0.98078528040323 -0.195090322016128,"
              "0.923879532511287 -0.38268343236509,"
              "0.831469612302545 -0.555570233019602,"
              "0.707106781186548 -0.707106781186547,"
              "0.555570233019602 -0.831469612302545,"
              "0.38268343236509 -0.923879532511287,"
              "0.195090322016128 -0.98078528040323,0 -1,"
              "-0.195090322016128 -0.98078528040323,"
              "-0.38268343236509 -0.923879532511287,"
              "-0.555570233019602 -0.831469612302545,"
              "-0.707106781186547 -0.707106781186548,"
              "-0.831469612302545 -0.555570233019602,"
              "-0.923879532511287 -0.38268343236509,"
              "-0.98078528040323 -0.195090322016129,-1 0,"
              "-0.98078528040323 0.195090322016128,"
              "-0.923879532511287 0.38268343236509,"
              "-0.831469612302545 0.555570233019602,"
              "-0.707106781186548 0.707106781186547,"
              "-0.555570233019602 0.831469612302545,"
              "-0.38268343236509 0.923879532511287,"
              "-0.195090322016129 0.98078528040323,0 1,"
              "0.195090322016128 0.98078528040323,"
              "0.38268343236509 0.923879532511287,"
              "0.555570233019602 0.831469612302545,"
              "0.707106781186547 0.707106781186548,"
              "0.831469612302545 0.555570233019602,"
              "0.923879532511287 0.38268343236509,"
              "0.98078528040323 0.195090322016129,1 0))"
          ),
      ),
      (
          "quadrant_segments",
          "POINT (0 0)",
          {"distance": 1, "quadrant-segments": 2},
          (
              "POLYGON ((1 0,0.707106781186548 -0.707106781186547,0 -1,"
              "-0.707106781186547 -0.707106781186548,-1 0,"
              "-0.707106781186548 0.707106781186547,0 1,"
              "0.707106781186547 0.707106781186548,1 0))"
          ),
      ),
      (
          "endcap_square",
          "POINT (0 0)",
          {"distance": 1, "endcap-style": "square"},
          "POLYGON ((1 1,1 -1,-1 -1,-1 1,1 1))",
      ),
      (
          "join_mitre",
          "LINESTRING (0 0,1 0,0 0.5)",
          {"distance": 1, "join-style": "mitre"},
          (
              "POLYGON ((-0.963907618884737 0.250728515642601,"
              "-0.992147591195 0.374927391943886,"
              "-0.997484208812642 0.570889020090679,"
              "-0.964488067681204 0.764126422950408,"
              "-0.894427190999916 0.947213595499958,"
              "-0.789993978969049 1.11311460037472,"
              "-0.655201741360129 1.25545395499571,"
              "-0.495230468272109 1.36876163778967,"
              "-0.316227766016838 1.44868329805051,"
              "-0.125072608056114 1.492147591195,"
              "0.070889020090679 1.49748420881264,"
              "0.264126422950408 1.4644880676812,"
              "0.447213595499958 1.39442719099992,"
              "5.23606797749979 -1,0 -1,"
              "-0.195090322016129 -0.98078528040323,"
              "-0.38268343236509 -0.923879532511287,"
              "-0.555570233019602 -0.831469612302545,"
              "-0.707106781186548 -0.707106781186547,"
              "-0.831469612302545 -0.555570233019602,"
              "-0.923879532511287 -0.38268343236509,"
              "-0.98078528040323 -0.195090322016128,-1 0,"
              "-0.98078528040323 0.195090322016129,"
              "-0.963907618884737 0.250728515642601))"
          ),
      ),
      (
          "mitre_limit",
          "LINESTRING (0 0,1 0,0 0.5)",
          {"distance": 1, "join-style": "mitre", "mitre-limit": 1},
          (
              "POLYGON ((-0.963907618884737 0.250728515642601,"
              "-0.992147591195 0.374927391943886,"
              "-0.997484208812642 0.570889020090679,"
              "-0.964488067681204 0.764126422950408,"
              "-0.894427190999916 0.947213595499958,"
              "-0.789993978969049 1.11311460037472,"
              "-0.655201741360129 1.25545395499571,"
              "-0.495230468272109 1.36876163778967,"
              "-0.316227766016838 1.44868329805051,"
              "-0.125072608056114 1.492147591195,"
              "0.070889020090679 1.49748420881264,"
              "0.264126422950408 1.4644880676812,"
              "0.447213595499958 1.39442719099992,"
              "2.15507965968923 0.540494158905278,"
              "1.79141831924623 -1,0 -1,"
              "-0.195090322016129 -0.98078528040323,"
              "-0.38268343236509 -0.923879532511287,"
              "-0.555570233019602 -0.831469612302545,"
              "-0.707106781186548 -0.707106781186547,"
              "-0.831469612302545 -0.555570233019602,"
              "-0.923879532511287 -0.38268343236509,"
              "-0.98078528040323 -0.195090322016128,-1 0,"
              "-0.98078528040323 0.195090322016129,"
              "-0.963907618884737 0.250728515642601))"
          ),
      ),
      (
          "side_left",
          "LINESTRING (0 0,1 0)",
          {"distance": 1, "side": "left"},
          "POLYGON ((1 0,0 0,0 1,1 1,1 0))",
      ),
      (
          "side_right",
          "LINESTRING (0 0,1 0)",
          {"distance": 1, "side": "right"},
          "POLYGON ((0 0,1 0,1 -1,0 -1,0 0))",
      ),
  )
  def test_vector_buffer(
      self,
      input_wkt: str,
      options: dict[str, Any],
      output_wkt: str,
  ) -> None:
    if not _has_geos(3, 11):
      self.skipTest("GEOS >= 3.11.0 not available")

    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    src_lyr = src_ds.CreateLayer("the_layer", srs=srs)

    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt(input_wkt))
    src_lyr.CreateFeature(feat)

    empty_feat = ogr.Feature(src_lyr.GetLayerDefn())
    src_lyr.CreateFeature(empty_feat)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    for key, value in options.items():
      alg[key] = value

    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    out_feat = out_lyr.GetNextFeature()
    out_geom = out_feat.GetGeometryRef()
    self.assertIsNotNone(out_geom)
    self._assert_geometry_matches(
        out_geom, ogr.CreateGeometryFromWkt(output_wkt)
    )
    self.assertEqual(
        out_geom.GetSpatialReference().GetAuthorityCode(None), "32631"
    )

    next_feat = out_lyr.GetNextFeature()
    self.assertIsNone(next_feat.GetGeometryRef())


if __name__ == "__main__":
  absltest.main()
