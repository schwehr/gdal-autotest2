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

"""Tests for `gdal raster edit`.

Ported from upstream autotest/utilities/test_gdalalg_raster_edit.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_edit_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["edit"]


def _get_pipeline_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  return reg.InstantiateAlg("raster").InstantiateSubAlgorithm("pipeline")


def _copy_byte_to_vsimem(vsimem_dir: str, name: str = "tmp.tif") -> str:
  byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
  dst_path = f"{vsimem_dir}/{name}"
  with open(byte_tif, "rb") as src:
    gdal.FileFromMemBuffer(dst_path, src.read())
  return dst_path


class GdalalgRasterEditTest(gdaltest.TestCase):

  def test_read_only(self) -> None:
    with utilities_util.vsimem_dir("raster_edit_ro") as tmp:
      tmp_filename = _copy_byte_to_vsimem(tmp)
      with gdal.OpenEx(tmp_filename) as ds:
        alg = _get_edit_alg()
        alg["dataset"] = ds
        with self.assertRaisesRegex(
            RuntimeError, "edit: Dataset should be opened in update mode"
        ):
          alg.Run()

  def test_crs_and_crs_none(self) -> None:
    with utilities_util.vsimem_dir("raster_edit_crs") as tmp:
      tmp_filename = _copy_byte_to_vsimem(tmp)
      alg = _get_edit_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(["--crs=EPSG:32611", tmp_filename])
      )
      with gdal.OpenEx(tmp_filename) as ds:
        srs = ds.GetSpatialRef()
        self.assertIsNotNone(srs)
        assert srs is not None
        self.assertEqual(srs.GetAuthorityCode(None), "32611")

      alg = _get_edit_alg()
      self.assertTrue(alg.ParseRunAndFinalize(["--crs=none", tmp_filename]))
      with gdal.OpenEx(tmp_filename) as ds:
        self.assertIsNone(ds.GetSpatialRef())

  def test_bbox_and_bbox_invalid(self) -> None:
    with utilities_util.vsimem_dir("raster_edit_bbox") as tmp:
      tmp_filename = _copy_byte_to_vsimem(tmp)
      alg = _get_edit_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(["--bbox=1,2,10,200", tmp_filename])
      )
      with gdal.OpenEx(tmp_filename) as ds:
        gt = ds.GetGeoTransform()
        expected = (1.0, 0.45, 0.0, 200.0, 0.0, -9.9)
        for got_val, exp_val in zip(gt, expected):
          self.assertAlmostEqual(got_val, exp_val)

      alg = _get_edit_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "Value of 'bbox' should be xmin,ymin,xmax,ymax with xmin <= xmax"
          " and ymin <= ymax",
      ):
        alg.ParseRunAndFinalize(["--bbox=1,200,10,2", tmp_filename])

  def test_nodata_and_nodata_invalid(self) -> None:
    with utilities_util.vsimem_dir("raster_edit_nodata") as tmp:
      tmp_filename = _copy_byte_to_vsimem(tmp)
      alg = _get_edit_alg()
      self.assertTrue(alg.ParseRunAndFinalize(["--nodata=100", tmp_filename]))
      with gdal.OpenEx(tmp_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 100)

      alg = _get_edit_alg()
      self.assertTrue(alg.ParseRunAndFinalize(["--nodata=none", tmp_filename]))
      with gdal.OpenEx(tmp_filename) as ds:
        self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())

      alg = _get_edit_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "Value of 'nodata' should be 'none', a numeric value, 'nan', 'inf'"
          " or '-inf'",
      ):
        alg.ParseRunAndFinalize(["--nodata=invalid", tmp_filename])

  def test_metadata_and_unset_metadata(self) -> None:
    with utilities_util.vsimem_dir("raster_edit_md") as tmp:
      tmp_filename = _copy_byte_to_vsimem(tmp)
      alg = _get_edit_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize([
              "--metadata",
              "foo=bar",
              "--metadata",
              "bar=baz",
              tmp_filename,
          ])
      )
      with gdal.OpenEx(tmp_filename) as ds:
        self.assertEqual(
            ds.GetMetadata(),
            {"AREA_OR_POINT": "Area", "foo": "bar", "bar": "baz"},
        )

      alg = _get_edit_alg()
      self.assertTrue(
          alg.ParseRunAndFinalize(["--unset-metadata", "foo", tmp_filename])
      )
      with gdal.OpenEx(tmp_filename) as ds:
        self.assertEqual(
            ds.GetMetadata(), {"AREA_OR_POINT": "Area", "bar": "baz"}
        )

  def test_unset_metadata_domain(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    src_ds.SetMetadata(['{ "key": "value" }'], "json:ISIS3")
    with utilities_util.vsimem_dir("raster_edit_mdd") as tmp:
      my_tif = f"{tmp}/my.tif"
      gdal.Translate(my_tif, src_ds)
      with gdal.Open(my_tif) as ds:
        self.assertEqual(ds.GetMetadata("json:ISIS3")[0], '{ "key": "value" }')

      gdal.Run(
          "raster edit", dataset=my_tif, unset_metadata_domain="json:ISIS3"
      )
      with gdal.Open(my_tif) as ds:
        self.assertIsNone(ds.GetMetadata("json:ISIS3"))

  def test_stats_and_approx_stats(self) -> None:
    for arg in ("stats", "approx-stats"):
      src_ds = gdal.GetDriverByName("MEM").Create("", 2, 1)
      src_ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\x02")
      alg = _get_edit_alg()
      alg["dataset"] = src_ds
      alg[arg] = True
      self.assertTrue(alg.Run())
      band = src_ds.GetRasterBand(1)
      self.assertEqual(band.GetMetadataItem("STATISTICS_MINIMUM"), "1")
      self.assertEqual(band.GetMetadataItem("STATISTICS_MAXIMUM"), "2")
      self.assertEqual(band.GetMetadataItem("STATISTICS_MEAN"), "1.5")
      self.assertEqual(band.GetMetadataItem("STATISTICS_STDDEV"), "0.5")

  def test_hist(self) -> None:
    with gdaltest.config_option("GDAL_PAM_ENABLED", "YES"):
      with utilities_util.vsimem_dir("raster_edit_hist") as tmp:
        tmp_filename = f"{tmp}/out.tif"
        with gdal.GetDriverByName("GTiff").Create(tmp_filename, 2, 1) as ds:
          ds.GetRasterBand(1).WriteRaster(0, 0, 2, 1, b"\x01\x02")

        with gdal.Open(tmp_filename) as ds:
          alg = _get_edit_alg()
          alg["dataset"] = ds
          alg["hist"] = True
          alg["auxiliary"] = True
          self.assertTrue(alg.Run())
          self.assertTrue(alg.Finalize())

        aux_path = f"{tmp_filename}.aux.xml"
        self.assertIsNotNone(gdal.VSIStatL(aux_path))
        with gdal.VSIFile(aux_path, "rb") as f:
          self.assertIn(b"<Histograms>", f.read())

  def test_pipeline_edit_crs_and_none(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_edit_pipe_crs") as tmp:
      out_filename = f"{tmp}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "edit",
              "--crs=EPSG:32611",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        srs = ds.GetSpatialRef()
        self.assertIsNotNone(srs)
        assert srs is not None
        self.assertEqual(srs.GetAuthorityCode(None), "32611")
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "edit",
              "--crs=none",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertIsNone(ds.GetSpatialRef())

  def test_pipeline_edit_bbox_nodata_and_metadata(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_edit_pipe_misc") as tmp:
      out_filename = f"{tmp}/out.tif"

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "edit",
              "--bbox=1,2,10,200",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        gt = ds.GetGeoTransform()
        expected = (1.0, 0.45, 0.0, 200.0, 0.0, -9.9)
        for got_val, exp_val in zip(gt, expected):
          self.assertAlmostEqual(got_val, exp_val)

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "edit",
              "--nodata=100",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 100)

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "edit",
              "--nodata=none",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "edit",
              "--metadata=foo=bar,bar=baz",
              "!",
              "edit",
              "--unset-metadata=foo",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(
            ds.GetMetadata(), {"AREA_OR_POINT": "Area", "bar": "baz"}
        )

  def test_gcp_from_list_of_values(self) -> None:
    mem_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    gdal.Run(
        "raster",
        "edit",
        dataset=mem_ds,
        crs="EPSG:4326",
        gcp=[[1.5, 2.5, 3.5, 4.5], [5.5, 6.5, 7.5, 8.5, 9.5]],
    )
    self.assertEqual(mem_ds.GetGCPCount(), 2)
    self.assertEqual(mem_ds.GetGCPSpatialRef().GetAuthorityCode(None), "4326")
    gcps = mem_ds.GetGCPs()
    self.assertEqual(gcps[0].GCPPixel, 1.5)
    self.assertEqual(gcps[0].GCPLine, 2.5)
    self.assertEqual(gcps[0].GCPX, 3.5)
    self.assertEqual(gcps[0].GCPY, 4.5)
    self.assertEqual(gcps[1].GCPPixel, 5.5)
    self.assertEqual(gcps[1].GCPLine, 6.5)
    self.assertEqual(gcps[1].GCPX, 7.5)
    self.assertEqual(gcps[1].GCPY, 8.5)
    self.assertEqual(gcps[1].GCPZ, 9.5)

  def test_gcp_from_list_of_gdal_gcp(self) -> None:
    mem_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    gdal.Run(
        "raster",
        "edit",
        dataset=mem_ds,
        gcp=[
            gdal.GCP(3.5, 4.5, 0, 1.5, 2.5),
            gdal.GCP(7.5, 8.5, 9.5, 5.5, 6.5),
        ],
    )
    self.assertEqual(mem_ds.GetGCPCount(), 2)
    self.assertIsNone(mem_ds.GetGCPSpatialRef())
    gcps = mem_ds.GetGCPs()
    self.assertEqual(gcps[0].GCPPixel, 1.5)
    self.assertEqual(gcps[0].GCPLine, 2.5)
    self.assertEqual(gcps[0].GCPX, 3.5)
    self.assertEqual(gcps[0].GCPY, 4.5)
    self.assertEqual(gcps[1].GCPPixel, 5.5)
    self.assertEqual(gcps[1].GCPLine, 6.5)
    self.assertEqual(gcps[1].GCPX, 7.5)
    self.assertEqual(gcps[1].GCPY, 8.5)
    self.assertEqual(gcps[1].GCPZ, 9.5)

  def test_gcp_from_vector_dataset(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    mem_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    with utilities_util.vsimem_dir("raster_edit_gcp_csv") as tmp:
      csv_path = f"{tmp}/gcps.csv"
      with gdal.GetDriverByName("CSV").CreateVector(csv_path) as gcp_ds:
        lyr = gcp_ds.CreateLayer("gcps")
        for field_name in ("id", "info", "column", "line", "x", "y", "z"):
          lyr.CreateField(ogr.FieldDefn(field_name))
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat["id"] = "my_id"
        feat["info"] = "my_info"
        feat["column"] = 1.5
        feat["line"] = 2.5
        feat["x"] = 3.5
        feat["y"] = 4.5
        feat["z"] = 5.5
        lyr.CreateFeature(feat)

      gdal.Run("raster", "edit", dataset=mem_ds, gcp=f"@{csv_path}")
      self.assertEqual(mem_ds.GetGCPCount(), 1)
      self.assertIsNone(mem_ds.GetGCPSpatialRef())
      gcp = mem_ds.GetGCPs()[0]
      self.assertEqual(gcp.Id, "my_id")
      self.assertEqual(gcp.Info, "my_info")
      self.assertEqual(gcp.GCPPixel, 1.5)
      self.assertEqual(gcp.GCPLine, 2.5)
      self.assertEqual(gcp.GCPX, 3.5)
      self.assertEqual(gcp.GCPY, 4.5)
      self.assertEqual(gcp.GCPZ, 5.5)

  def test_gcp_bad_format_and_missing_vector(self) -> None:
    mem_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    with self.assertRaisesRegex(RuntimeError, " Bad format for 1,2,3"):
      gdal.Run("raster", "edit", dataset=mem_ds, gcp="1,2,3")

    with self.assertRaisesRegex(RuntimeError, " Bad format for 1,2,3,foo"):
      gdal.Run("raster", "edit", dataset=mem_ds, gcp="1,2,3,foo")

    with self.assertRaisesRegex(RuntimeError, "/i_do/not/exist.csv"):
      gdal.Run("raster", "edit", dataset=mem_ds, gcp="@/i_do/not/exist.csv")

  def test_gcp_from_gpkg_errors(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    mem_ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    with utilities_util.vsimem_dir("raster_edit_gcp_gpkg") as tmp:
      two_layers_gpkg = f"{tmp}/two_layers.gpkg"
      with gdal.GetDriverByName("GPKG").CreateVector(two_layers_gpkg) as ds:
        ds.CreateLayer("a")
        ds.CreateLayer("b")
      with self.assertRaisesRegex(
          RuntimeError, "GCPs can only be specified for single-layer datasets"
      ):
        gdal.Run("raster", "edit", dataset=mem_ds, gcp=f"@{two_layers_gpkg}")

      missing_col_gpkg = f"{tmp}/missing_col.gpkg"
      with gdal.GetDriverByName("GPKG").CreateVector(missing_col_gpkg) as ds:
        lyr = ds.CreateLayer("gcps")
        lyr.CreateField(ogr.FieldDefn("line"))
        lyr.CreateField(ogr.FieldDefn("x"))
        lyr.CreateField(ogr.FieldDefn("y"))
      with self.assertRaisesRegex(
          RuntimeError, "Field 'column' cannot be found in"
      ):
        gdal.Run("raster", "edit", dataset=mem_ds, gcp=f"@{missing_col_gpkg}")

  def test_gcp_output_format_does_not_support(self) -> None:
    if not utilities_util.has_driver("Zarr"):
      self.skipTest("Zarr driver not available")
    with utilities_util.vsimem_dir("raster_edit_gcp_zarr") as tmp:
      ds = gdal.GetDriverByName("ZARR").Create(f"{tmp}/test.zarr", 1, 1)
      with self.assertRaisesRegex(RuntimeError, "Setting GCPs failed"):
        gdal.Run("raster", "edit", dataset=ds, gcp=[[1.5, 2.5, 3.5, 4.5]])


if __name__ == "__main__":
  absltest.main()
