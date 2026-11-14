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

"""Tests for `gdal raster footprint`.

Ported from upstream autotest/utilities/test_gdalalg_raster_footprint.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["footprint"]


def _have_geos() -> bool:
  point1 = ogr.CreateGeometryFromWkt("POINT (10 20)")
  point2 = ogr.CreateGeometryFromWkt("POINT (30 20)")
  try:
    with gdal.quiet_errors():
      return point1.Union(point2) is not None
  except RuntimeError:
    return False


class GdalalgRasterFootprintTest(gdaltest.TestCase, parameterized.TestCase):

  def test_basic(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    last_pct = [0.0]

    def my_progress(pct: float, _msg: str, _user_data: object) -> bool:
      last_pct[0] = pct
      return True

    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run(my_progress))
    self.assertEqual(last_pct[0], 1.0)
    ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = ds.GetLayerByName("footprint")
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "26711")
    self.assertEqual(lyr.GetFeatureCount(), 1)
    feat = lyr.GetNextFeature()
    self.assertEqual(feat["location"], byte_tif)
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((440720 3751320,440720 3750120,"
        "441920 3750120,441920 3751320,440720 3751320)))",
    )

  def test_existing_output(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_footprint_existing") as tmp:
      out_filename = f"{tmp}/out.shp"

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["append"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 2)
      self.assertTrue(alg.Finalize())
      ds.Close()

      alg = _get_alg()
      alg["input"] = byte_tif
      out_ds = gdal.OpenEx(out_filename, gdal.OF_UPDATE)
      alg["output"] = out_ds
      alg["append"] = True
      self.assertTrue(alg.Run())
      self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 3)
      self.assertTrue(alg.Finalize())
      out_ds.Close()

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      ds = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)
      self.assertTrue(alg.Finalize())
      ds.Close()

  def test_output_layer(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "foo"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertIsNotNone(ds.GetLayerByName("foo"))

  def test_creation_options(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_footprint_co") as tmp:
      out_filename = f"{tmp}/out.gpkg"

      alg = _get_alg()
      alg["input"] = byte_tif
      alg["output"] = out_filename
      alg["creation-option"] = {"METADATA_TABLES": "YES"}
      alg["layer-creation-option"] = {"DESCRIPTION": "my_description"}
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.OpenEx(out_filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(
            ds.GetLayer(0).GetMetadata(), {"DESCRIPTION": "my_description"}
        )
        with ds.ExecuteSQL(
            "SELECT * FROM sqlite_master WHERE name LIKE '%metadata%'"
        ) as sql_lyr:
          self.assertEqual(sql_lyr.GetFeatureCount(), 2)

  def test_band_selection_and_combine(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 4, 1, 3)
    src_ds.GetRasterBand(1).SetNoDataValue(0)
    src_ds.GetRasterBand(2).WriteRaster(0, 0, 2, 1, b"\x01\x01")
    src_ds.GetRasterBand(2).SetNoDataValue(0)
    src_ds.GetRasterBand(3).WriteRaster(1, 0, 2, 1, b"\x01\x01")
    src_ds.GetRasterBand(3).SetNoDataValue(0)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["output-layer"] = "foo"
    alg["band"] = 1
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertIsNone(out_ds.GetLayer(0).GetNextFeature())

    for band, combine, expected_wkt in (
        (2, None, "MULTIPOLYGON (((0 0,0 1,2 1,2 0,0 0)))"),
        ([2, 3], None, "MULTIPOLYGON (((0 0,0 1,3 1,3 0,0 0)))"),
        ([2, 3], "intersection", "MULTIPOLYGON (((1 0,1 1,2 1,2 0,1 0)))"),
    ):
      alg = _get_alg()
      alg["input"] = src_ds
      alg["output"] = ""
      alg["output-format"] = "MEM"
      alg["output-layer"] = "foo"
      alg["band"] = band
      if combine is not None:
        alg["combine-bands"] = combine
      self.assertTrue(alg.Run())
      out_ds = utilities_util.get_arg_dataset(alg, "output")
      feat = out_ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), expected_wkt)

  def test_overview(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    src_ds.GetRasterBand(1).Fill(0)
    src_ds.GetRasterBand(1).SetNoDataValue(0)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["overview"] = 0
    with self.assertRaisesRegex(
        RuntimeError,
        "Source dataset has no overviews. Argument 'overview' should not be"
        " specified",
    ):
      alg.Run()

    src_ds.BuildOverviews("NONE", [2])
    src_ds.GetRasterBand(1).GetOverview(0).Fill(255)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["overview"] = 0
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0 2,2 2,2 0,0 0)))",
    )

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["overview"] = 1
    with self.assertRaisesRegex(
        RuntimeError,
        "Source dataset has only 1 overview levels. 'overview' value should be"
        " strictly lower than this number",
    ):
      alg.Run()

  def test_src_nodata(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.GetRasterBand(1).Fill(255)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["src-nodata"] = 255
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertIsNone(out_ds.GetLayer(0).GetNextFeature())

  @parameterized.parameters(True, False)
  def test_src_nodata_several(self, use_set_nodata_value: bool) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 1, 2)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 1, 1, b"\x01")
    src_ds.GetRasterBand(2).WriteRaster(0, 0, 1, 1, b"\x01")
    if use_set_nodata_value:
      src_ds.GetRasterBand(1).SetNoDataValue(0)
      src_ds.GetRasterBand(2).SetNoDataValue(1)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    if not use_set_nodata_value:
      alg["src-nodata"] = [0, 1]
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0 1,2 1,2 0,0 0)))",
    )

  def test_coordinate_system_and_dst_crs(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.SetGeoTransform([2, 1, 0, 49, 0, -1])
    src_ds.GetRasterBand(1).Fill(255)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((2 49,2 48,3 48,3 49,2 49)))",
    )

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["coordinate-system"] = "pixel"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))",
    )

    no_gt_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    no_gt_ds.GetRasterBand(1).Fill(255)
    alg = _get_alg()
    alg["input"] = no_gt_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["coordinate-system"] = "georeferenced"
    with self.assertRaisesRegex(
        RuntimeError,
        "Georeferenced coordinates requested, but input dataset has no"
        " geotransform",
    ):
      alg.Run()

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["dst-crs"] = "EPSG:4267"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = out_ds.GetLayer(0)
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "4267")
    feat = lyr.GetNextFeature()
    self.assertTrue(
        feat.GetGeometryRef()
        .ExportToWkt()
        .startswith("MULTIPOLYGON (((-117.6411")
    )

  def test_split_multipolygons(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 2, 2, b"\x01\x00\x00\x01")
    src_ds.GetRasterBand(1).SetNoDataValue(0)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)),((1 1,1 2,2 2,2 1,1 1)))",
    )

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["split-multipolygons"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = out_ds.GetLayer(0)
    feat1 = lyr.GetNextFeature()
    self.assertIsNotNone(feat1)
    self.assertEqual(
        feat1.GetGeometryRef().ExportToWkt(),
        "POLYGON ((0 0,0 1,1 1,1 0,0 0))",
    )
    feat2 = lyr.GetNextFeature()
    self.assertIsNotNone(feat2)
    self.assertEqual(
        feat2.GetGeometryRef().ExportToWkt(),
        "POLYGON ((1 1,1 2,2 2,2 1,1 1))",
    )

  def test_convex_hull(self) -> None:
    if not _have_geos():
      self.skipTest("GEOS not available")
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 2, 2, b"\x01\x00\x00\x01")
    src_ds.GetRasterBand(1).SetNoDataValue(0)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["convex-hull"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0 1,1 2,2 2,2 1,1 0,0 0)))",
    )

  def test_densify_and_min_ring_area(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.GetRasterBand(1).Fill(1)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["densify-distance"] = 0.5
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0.0 0.5,0 1,0.5 1.0,1 1,1.0 0.5,1 0,"
        "0.5 0.0,0 0)))",
    )

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["min-ring-area"] = 1.1
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertIsNone(out_ds.GetLayer(0).GetNextFeature())

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["min-ring-area"] = 0.9
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))",
    )

  def test_simplify(self) -> None:
    if not _have_geos():
      self.skipTest("GEOS not available")
    src_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    src_ds.GetRasterBand(1).WriteRaster(0, 0, 2, 2, b"\x01\x01\x00\x01")
    src_ds.GetRasterBand(1).SetNoDataValue(0)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["simplify-tolerance"] = 1
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,2 2,2 0,0 0)))",
    )

  def test_max_points(self) -> None:
    if not _have_geos():
      self.skipTest("GEOS not available")
    src_ds = gdal.GetDriverByName("MEM").Create("", 10, 9)
    src_ds.GetRasterBand(1).SetNoDataValue(0)
    for i in range(9):
      src_ds.GetRasterBand(1).WriteRaster(i, i, 2, 1, b"\x01\x01")

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["max-points"] = 30
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(
        feat.GetGeometryRef().ExportToWkt(),
        "MULTIPOLYGON (((0 0,8 9,10 9,2 0,0 0)))",
    )

    alg = _get_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "Value of 'max-points' should be a positive integer greater or equal to"
        " 4, or 'unlimited'",
    ):
      alg["max-points"] = "illegal"

  def test_location_field_and_absolute_path(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("bar", 1, 1)
    src_ds.GetRasterBand(1).Fill(1)

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["location-field"] = "foo"
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(feat["foo"], "bar")

    alg = _get_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["no-location-field"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    lyr = out_ds.GetLayer(0)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 0)

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_alg()
    alg["input"] = byte_tif
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["absolute-path"] = True
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertIn("byte.tif", feat["location"])
    self.assertTrue(os.path.isabs(feat["location"]))


if __name__ == "__main__":
  absltest.main()
