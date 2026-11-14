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

"""Tests for `gdal vector reproject`.

Ported from upstream autotest/utilities/test_gdalalg_vector_reproject.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os
import shutil
import tempfile

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


_POLY_PRJ_WKT = (
    'PROJCS["OSGB 1936 / British National Grid",'
    'GEOGCS["OSGB 1936",DATUM["D_OSGB_1936",'
    'SPHEROID["Airy_1830",6377563.396,299.3249646]],'
    'PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],'
    'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",49],'
    'PARAMETER["central_meridian",-2],PARAMETER["scale_factor",0.9996012717],'
    'PARAMETER["false_easting",400000],PARAMETER["false_northing",-100000],'
    'UNIT["Meter",1]]'
)


def _get_reproject_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["reproject"]


class GdalalgVectorReprojectTest(gdaltest.TestCase, parameterized.TestCase):

  def _check_geom_matches_wkt(
      self,
      actual: ogr.Geometry | None,
      expected_wkt: str,
      max_error: float = 0.0001,
  ) -> None:
    expected = ogr.CreateGeometryFromWkt(expected_wkt)
    self.assertIsNotNone(expected)
    self.assertIsNotNone(actual)
    self.assertEqual(actual.GetGeometryName(), expected.GetGeometryName())

    if (
        ogr.GetGEOSVersionMajor() > 0
        and not actual.Is3D()
        and not actual.IsMeasured()
        and actual.Within(expected)
        and expected.Within(actual)
    ):
      return

    self._assert_geom_points_close(actual, expected, max_error)

  def _assert_geom_points_close(
      self,
      actual: ogr.Geometry,
      expected: ogr.Geometry,
      max_error: float,
  ) -> None:
    self.assertEqual(actual.GetGeometryCount(), expected.GetGeometryCount())
    self.assertEqual(actual.GetPointCount(), expected.GetPointCount())
    if actual.GetGeometryCount() > 0:
      for idx in range(actual.GetGeometryCount()):
        self._assert_geom_points_close(
            actual.GetGeometryRef(idx), expected.GetGeometryRef(idx), max_error
        )
    else:
      for idx in range(actual.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(idx), expected.GetX(idx), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetY(idx), expected.GetY(idx), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetZ(idx), expected.GetZ(idx), delta=max_error
        )

  def test_dataset_getnextfeature(self) -> None:
    if not utilities_util.has_driver("OSM"):
      self.skipTest("OSM driver not available")
    test_pbf = utilities_util.get_testdata_path("../ogr/data/osm/test.pbf")
    src_ds = gdal.OpenEx(test_pbf)

    alg = _get_reproject_alg()
    alg["input"] = src_ds
    alg["dst-crs"] = "EPSG:4326"
    self.assertTrue(
        alg.ParseCommandLineArguments(
            ["--of", "stream", "--output", "streamed_output"]
        )
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertTrue(out_ds.TestCapability(ogr.ODsCRandomLayerRead))

    expected: list[str] = []
    while True:
      feat, _ = src_ds.GetNextFeature()
      if feat is None:
        break
      expected.append(str(feat))

    got: list[str] = []
    out_ds.ResetReading()
    while True:
      feat, _ = out_ds.GetNextFeature()
      if feat is None:
        break
      got.append(str(feat))

    self.assertEqual(got, expected)

  def test_active_layer(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    for layer_name in ("the_layer", "other_layer"):
      lyr = src_ds.CreateLayer(layer_name)
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (3 0)"))
      lyr.CreateFeature(feat)

    alg = _get_reproject_alg()
    alg["input"] = src_ds
    alg["active-layer"] = "the_layer"
    self.assertTrue(
        alg.ParseCommandLineArguments([
            "--src-crs=EPSG:4326",
            "--dst-crs=EPSG:32631",
            "--of",
            "MEM",
            "--output",
            "memory_ds",
        ])
    )
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat0 = out_ds.GetLayer(0).GetNextFeature()
    self._check_geom_matches_wkt(feat0.GetGeometryRef(), "POINT (500000 0)")

    feat1 = out_ds.GetLayer(1).GetNextFeature()
    self._check_geom_matches_wkt(feat1.GetGeometryRef(), "POINT (3 0)")

  def test_complete_dst_crs(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    base = os.path.splitext(poly_shp)[0]
    with tempfile.TemporaryDirectory() as tmpdir:
      for ext in (".shp", ".shx", ".dbf"):
        shutil.copyfile(base + ext, os.path.join(tmpdir, "poly" + ext))
      prj_src = next(
          (
              base + ext
              for ext in (".PRJ", ".prj")
              if os.path.exists(base + ext)
          ),
          None,
      )
      dst_prj = os.path.join(tmpdir, "poly.prj")
      if prj_src is not None:
        shutil.copyfile(prj_src, dst_prj)
      else:
        with open(dst_prj, "w", encoding="ascii") as f:
          f.write(_POLY_PRJ_WKT)

      staged_poly = os.path.join(tmpdir, "poly.shp")
      out, _, code = utilities_util.run_gdal_cli([
          "completion",
          "gdal",
          "vector",
          "reproject",
          staged_poly,
          "--dst-crs=EPSG:",
      ])
      if code == -6:
        self.skipTest(
            "Upstream GDALInConstructionAlgorithmArg::SetIsCRSArg std::sort "
            "comparator is not strict-weak-ordering under libc++ hardening; "
            "see gdalalgorithm.cpp:1618"
        )
      self.assertEqual(code, 0)
      self.assertIn("4326\\ --", out)
      self.assertNotIn("2193\\ --", out)

  @parameterized.parameters(
      (
          (
              "POLYGON((0 100000,100000 0,0 -100000,-100000 0,0 100000),"
              "(0 50000,50000 0,0 -50000,-50000 0,0 50000))"
          ),
          (
              "POLYGON ((90.0 89.089200825091,0.0 89.089200825091,"
              "-90 89.089200825091,-180 89.0892008251069,"
              "-180 89.5445935108883,-90 89.5445935108803,"
              "0.0 89.5445935108803,90.0 89.5445935108803,"
              "180.0 89.5445935108883,180.0 89.0892008251069,"
              "90.0 89.089200825091))"
          ),
      ),
      (
          (
              "POLYGON((50000 -100000,100000 -100000,100000 100000,"
              "-100000 100000,-100000 50000,50000 50000,50000 -100000))"
          ),
          (
              "MULTIPOLYGON (((135.0 88.7119614804959,45.0 88.7119614804959,"
              "26.565051177078 88.9817007095479,135.0 89.3559612202261,"
              "180.0 89.5445935108803,180.0 89.089200825091,"
              "135.0 88.7119614804959)),((-116.565051177078 88.9817007095479,"
              "-135 88.7119614804959,-180 89.089200825091,"
              "-180 89.5445935108803,-116.565051177078 88.9817007095479)))"
          ),
      ),
  )
  def test_polar_projected_to_geographic(
      self, input_wkt: str, output_wkt: str
  ) -> None:
    if ogr.GetGEOSVersionMajor() == 0:
      self.skipTest("GEOS not available")
    srs_3996 = osr.SpatialReference()
    srs_3996.ImportFromEPSG(3996)
    srs_3996.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    ds = gdal.GetDriverByName("MEM").CreateVector("")
    lyr = ds.CreateLayer("test", srs=srs_3996)
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt(input_wkt))
    lyr.CreateFeature(feat)

    with gdal.Run(
        "vector",
        "reproject",
        input=ds,
        output="",
        output_format="MEM",
        dst_crs="EPSG:4326",
    ) as alg:
      out_ds = alg.Output()
      out_feat = out_ds.GetLayer(0).GetNextFeature()
      self._check_geom_matches_wkt(out_feat.GetGeometryRef(), output_wkt)


if __name__ == "__main__":
  absltest.main()
