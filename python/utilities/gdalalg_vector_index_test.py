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

"""Tests for `gdal vector index`.

Ported from upstream autotest/utilities/test_gdalalg_vector_index.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib
import os
import tempfile

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_POLY_PRJ = (
    'PROJCS["OSGB 1936 / British National Grid",GEOGCS["OSGB 1936",'
    'DATUM["D_OSGB_1936",SPHEROID["Airy_1830",6377563.396,299.3249646]],'
    'PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],'
    'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",49],'
    'PARAMETER["central_meridian",-2],PARAMETER["scale_factor",0.9996012717],'
    'PARAMETER["false_easting",400000],PARAMETER["false_northing",-100000],'
    'UNIT["Meter",1]]'
)


def _copy_poly_with_prj(dst_dir: str) -> str:
  """Copies poly.{shp,dbf,shx} and writes poly.prj into dst_dir."""
  for ext in ("shp", "dbf", "shx"):
    src = utilities_util.get_testdata_path(f"../ogr/data/poly.{ext}")
    with open(src, "rb") as f_in:
      data = f_in.read()
    with gdal.VSIFile(os.path.join(dst_dir, f"poly.{ext}"), "wb") as f_out:
      f_out.write(data)
  with gdal.VSIFile(os.path.join(dst_dir, "poly.prj"), "wb") as f_prj:
    f_prj.write(_POLY_PRJ.encode("ascii"))
  return os.path.join(dst_dir, "poly.shp")


def setUpModule() -> None:
  utilities_util.setup_test_env()


@contextlib.contextmanager
def _capture_errors() -> Iterator[list[int]]:
  err_types: list[int] = []

  def _handler(err_class: int, unused_err_no: int, unused_msg: str) -> None:
    err_types.append(err_class)

  gdal.ErrorReset()
  gdal.PushErrorHandler(_handler)
  try:
    yield err_types
  finally:
    gdal.PopErrorHandler()


class GdalalgVectorIndexTest(gdaltest.TestCase):

  def test_new_file_and_progress(self) -> None:
    tab_pct = [0.0]

    def my_progress(
        pct: float, unused_msg: str, unused_user_data: object
    ) -> bool:
      self.assertGreaterEqual(pct, tab_pct[0])
      tab_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("vector_index_new") as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)
      with gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          metadata={"FOO": "BAR"},
          progress=my_progress,
      ) as alg:
        self.assertEqual(tab_pct[0], 1.0)
        ds = alg.Output()
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetName(), "tileindex")
        self.assertEqual(lyr.GetFeatureCount(), 1)
        self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "27700")
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["location"], f"{poly_shp},0")
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            "POLYGON ((478315.53125 4762880.5,478315.53125 4765610.5,"
            "481645.3125 4765610.5,481645.3125 4762880.5,"
            "478315.53125 4762880.5))",
        )
        self.assertEqual(lyr.GetMetadata_Dict(), {"FOO": "BAR"})

  def test_absolute_path(self) -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)
      with gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          absolute_path=True,
      ) as alg:
        ds = alg.Output()
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetName(), "tileindex")
        self.assertEqual(lyr.GetFeatureCount(), 1)
        self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "27700")
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        feat = lyr.GetNextFeature()
        expected = os.path.normpath(poly_shp).replace("\\", "/") + ",0"
        self.assertEqual(feat["location"].replace("\\", "/"), expected)

  def test_source_no_crs(self) -> None:
    with utilities_util.vsimem_dir("vector_index_no_crs") as tmpdir:
      no_crs = f"{tmpdir}/no_crs.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(no_crs) as ds:
        ds.CreateLayer("no_crs", geom_type=ogr.wkbPoint)

      with gdal.Run(
          "vector", "index", input=no_crs, output_format="MEM"
      ) as alg:
        ds = alg.Output()
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetName(), "tileindex")
        self.assertEqual(lyr.GetFeatureCount(), 1)
        self.assertIsNone(lyr.GetSpatialRef())
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)

  def test_source_as_dataset_and_bad_output_errors(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    src_ds = gdal.GetDriverByName("MEM").CreateVector("")
    with self.assertRaisesRegex(
        RuntimeError, "Input datasets must be provided by name, not as object"
    ):
      gdal.Run("vector", "index", input=src_ds, output_format="MEM")

    with self.assertRaisesRegex(RuntimeError, "Failed to create file"):
      gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output="/i_can/not/be/created.shp",
      )

  def test_dst_crs(self) -> None:
    with utilities_util.vsimem_dir("vector_index_dst_crs") as tmpdir:
      src_path = f"{tmpdir}/dst_crs.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src_path) as ds:
        lyr = ds.CreateLayer("dst_crs", srs=osr.SpatialReference(epsg=32631))
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT(500000 0)"))
        lyr.CreateFeature(feat)

      with gdal.Run(
          "vector",
          "index",
          input=src_path,
          output_format="MEM",
          dst_crs="EPSG:4326",
      ) as alg:
        ds = alg.Output()
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1)
        self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "4326")
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        feat = lyr.GetNextFeature()
        geom = feat.GetGeometryRef()
        self.assertEqual(geom.GetGeometryName(), "POLYGON")
        ring = geom.GetGeometryRef(0)
        self.assertEqual(ring.GetPointCount(), 5)
        for idx in range(5):
          self.assertAlmostEqual(ring.GetX(idx), 3.0, delta=1e-4)
          self.assertAlmostEqual(ring.GetY(idx), 0.0, delta=1e-4)

  def test_store_source_crs_formats(self) -> None:
    with utilities_util.vsimem_dir("vector_index_crs_fmt") as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)

      with gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          source_crs_field_name="crs",
      ) as alg:
        out_ds = alg.Output()
        lyr = out_ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat["crs"], "EPSG:27700")

      src_path = f"{tmpdir}/test.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src_path) as ds:
        ds.CreateLayer(
            "test", srs=osr.SpatialReference("+proj=utm +zone=31 +ellps=GRS80")
        )

      with gdal.Run(
          "vector",
          "index",
          input=src_path,
          output_format="MEM",
          source_crs_field_name="crs",
      ) as alg:
        out_ds = alg.Output()
        lyr = out_ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
        feat = lyr.GetNextFeature()
        self.assertTrue(feat["crs"].startswith("PROJCS["))

      idx_path = f"{tmpdir}/index.gpkg"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(idx_path) as ds:
        lyr = ds.CreateLayer(
            "tileindex",
            srs=osr.SpatialReference("+proj=utm +zone=31 +ellps=GRS80"),
        )
        lyr.CreateField(ogr.FieldDefn("location"))
        fld_defn = ogr.FieldDefn("crs")
        fld_defn.SetWidth(50)
        lyr.CreateField(fld_defn)

      with gdal.Run(
          "vector",
          "index",
          input=src_path,
          output=idx_path,
          append=True,
          source_crs_field_name="crs",
      ) as alg:
        out_ds = alg.Output()
        lyr = out_ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
        feat = lyr.GetNextFeature()
        self.assertTrue(feat["crs"].startswith("+proj=utm "))

      for fmt, prefix in (
          ("EPSG", "EPSG:27700"),
          ("WKT", "PROJCS"),
          ("PROJ", "+proj=tmerc "),
      ):
        with gdal.Run(
            "vector",
            "index",
            input=poly_shp,
            output_format="MEM",
            source_crs_field_name="crs",
            source_crs_format=fmt,
        ) as alg:
          out_ds = alg.Output()
          lyr = out_ds.GetLayer(0)
          self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
          feat = lyr.GetNextFeature()
          self.assertTrue(feat["crs"].startswith(prefix))

  def test_location_and_source_crs_field_names(self) -> None:
    with utilities_util.vsimem_dir("vector_index_fld_names") as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)
      alg = gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          location_name="my_loc",
          source_crs_field_name="my_crs",
      )
      ds = alg.Output()
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat["my_loc"], f"{poly_shp},0")
      self.assertEqual(feat["my_crs"], "EPSG:27700")

      with self.assertRaisesRegex(
          RuntimeError,
          "Unable to find field 'wrong_location_name' in output layer",
      ):
        gdal.Run(
            "vector",
            "index",
            input=poly_shp,
            output=ds,
            location_name="wrong_location_name",
            source_crs_field_name="my_crs",
            append=True,
        )

      with self.assertRaisesRegex(
          RuntimeError,
          "Unable to find field 'wrong_source_crs_field_name' in output layer",
      ):
        gdal.Run(
            "vector",
            "index",
            input=poly_shp,
            output=ds,
            location_name="my_loc",
            source_crs_field_name="wrong_source_crs_field_name",
            append=True,
        )

  def test_store_source_crs_wkt_too_long(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with utilities_util.vsimem_dir("vector_index_wkt_long") as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)
      idx_path = f"{tmpdir}/index.gpkg"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(idx_path) as ds:
        lyr = ds.CreateLayer("tileindex", srs=osr.SpatialReference(epsg=27700))
        lyr.CreateField(ogr.FieldDefn("location"))
        fld_defn = ogr.FieldDefn("crs")
        fld_defn.SetWidth(10)
        lyr.CreateField(fld_defn)

      with _capture_errors() as errs:
        with gdal.Run(
            "vector",
            "index",
            input=poly_shp,
            output=idx_path,
            append=True,
            source_crs_field_name="crs",
            source_crs_format="WKT",
        ) as alg:
          out_ds = alg.Output()
          lyr = out_ds.GetLayer(0)
          self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
          feat = lyr.GetNextFeature()
          self.assertIsNone(feat["crs"])
      self.assertIn(gdal.CE_Warning, errs)

  def test_append_and_fid_field_failures_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    poly_gpkg_zip = utilities_util.get_testdata_path(
        "../ogr/data/gpkg/poly.gpkg.zip"
    )
    with utilities_util.vsimem_dir("vector_index_gpkg") as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)
      out_shp = f"{tmpdir}/out.shp"
      self.assertIsNotNone(
          gdal.Run("vector", "index", input=poly_shp, output=out_shp)
      )
      with _capture_errors() as errs:
        self.assertIsNotNone(
            gdal.Run(
                "vector",
                "index",
                input=poly_shp,
                output=out_shp,
                accept_different_schemas=True,
                append=True,
            )
        )
      self.assertIn(gdal.CE_Warning, errs)
      with gdal.OpenEx(out_shp) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

      self.assertIsNotNone(
          gdal.Run(
              "vector",
              "index",
              input=poly_gpkg_zip,
              output=out_shp,
              accept_different_schemas=True,
              append=True,
          )
      )
      with gdal.OpenEx(out_shp) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 2)

      with self.assertRaises(RuntimeError):
        gdal.Run(
            "vector",
            "index",
            input=poly_shp,
            output=f"{tmpdir}/bad_loc.gpkg",
            location_name="fid",
        )

      with self.assertRaises(RuntimeError):
        gdal.Run(
            "vector",
            "index",
            input=poly_shp,
            output=f"{tmpdir}/bad_crs.gpkg",
            source_crs_field_name="fid",
        )

  def test_different_crs_and_schemas(self) -> None:
    with utilities_util.vsimem_dir("vector_index_diff") as tmpdir:
      src_path = f"{tmpdir}/test.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src_path) as ds:
        ds.CreateLayer(
            "test", srs=osr.SpatialReference("+proj=utm +zone=31 +ellps=GRS80")
        )

      idx_path = f"{tmpdir}/index.gpkg"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(idx_path) as ds:
        lyr = ds.CreateLayer(
            "tileindex",
            srs=osr.SpatialReference("+proj=utm +zone=30 +ellps=GRS80"),
        )
        lyr.CreateField(ogr.FieldDefn("location"))

      with _capture_errors() as errs:
        with gdal.Run(
            "vector", "index", input=src_path, output=idx_path, append=True
        ) as alg:
          out_ds = alg.Output()
          self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 0)
      self.assertIn(gdal.CE_Warning, errs)

      with _capture_errors() as errs:
        with gdal.Run(
            "vector",
            "index",
            input=src_path,
            output=idx_path,
            append=True,
            accept_different_crs=True,
        ) as alg:
          out_ds = alg.Output()
          self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 1)
      self.assertIn(gdal.CE_Warning, errs)

      src1_path = f"{tmpdir}/test1.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src1_path) as ds:
        ds.CreateLayer("test1")

      src2_path = f"{tmpdir}/test2.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src2_path) as ds:
        lyr = ds.CreateLayer("test2")
        lyr.CreateField(ogr.FieldDefn("foo"))

      src3_path = f"{tmpdir}/test3.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src3_path) as ds:
        lyr = ds.CreateLayer("test3")
        lyr.CreateField(ogr.FieldDefn("bar"))

      out_idx = f"{tmpdir}/index_schema.shp"
      with _capture_errors() as errs:
        with gdal.Run(
            "vector", "index", input=[src1_path, src2_path], output=out_idx
        ) as alg:
          out_ds = alg.Output()
          self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 1)
      self.assertIn(gdal.CE_Warning, errs)

      with gdal.Run(
          "vector",
          "index",
          input=[src1_path, src2_path, src3_path],
          output=out_idx,
          overwrite=True,
          accept_different_schemas=True,
      ) as alg:
        out_ds = alg.Output()
        self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 3)

  def test_empty_source_and_reprojection_failed(self) -> None:
    with utilities_util.vsimem_dir("vector_index_edge") as tmpdir:
      dbf_path = f"{tmpdir}/aspatial.dbf"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(dbf_path) as ds:
        ds.CreateLayer("aspatial", geom_type=ogr.wkbNone)

      with _capture_errors() as errs:
        with gdal.Run(
            "vector", "index", input=dbf_path, output_format="MEM"
        ) as alg:
          out_ds = alg.Output()
          self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 0)
      self.assertIn(gdal.CE_Warning, errs)

      src_path = f"{tmpdir}/test.shp"
      with gdal.GetDriverByName("ESRI Shapefile").CreateVector(src_path) as ds:
        ds.CreateLayer(
            "test", srs=osr.SpatialReference("+proj=longlat +datum=WGS84")
        )

      with _capture_errors() as errs:
        with gdal.Run(
            "vector",
            "index",
            input=src_path,
            output=f"{tmpdir}/index.shp",
            source_crs_field_name="crs",
            dst_crs="+proj=longlat +a=1",
        ) as alg:
          out_ds = alg.Output()
          self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 0)
      self.assertIn(gdal.CE_Warning, errs)

  def test_source_layer_selection_and_incompatible_options(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    with gdal.Run(
        "vector",
        "index",
        input=poly_shp,
        output_format="MEM",
        source_layer_name="poly",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 1)

    with self.assertRaisesRegex(RuntimeError, "No layer to index"):
      gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          source_layer_name="unknown",
      )

    with gdal.Run(
        "vector",
        "index",
        input=poly_shp,
        output_format="MEM",
        source_layer_index=0,
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.GetLayer(0).GetFeatureCount(), 1)

    with self.assertRaisesRegex(RuntimeError, "No layer to index"):
      gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          source_layer_index=1,
      )

    with self.assertRaises(RuntimeError):
      gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          accept_different_crs=True,
          skip_different_crs=True,
      )

    with self.assertRaises(RuntimeError):
      gdal.Run(
          "vector",
          "index",
          input=poly_shp,
          output_format="MEM",
          source_crs_format="EPSG",
      )

    with _capture_errors() as errs:
      self.assertIsNotNone(
          gdal.Run(
              "vector",
              "index",
              input=poly_shp,
              output_format="MEM",
              dst_crs="EPSG:4326",
              skip_different_crs=True,
          )
      )
    self.assertIn(gdal.CE_Warning, errs)

  def test_filename_filter(self) -> None:
    ogr_dir = os.path.dirname(
        utilities_util.get_testdata_path("../ogr/data/poly.shp")
    )
    with gdal.Run(
        "vector",
        "index",
        input=ogr_dir,
        output_format="MEM",
        recursive=True,
        source_layer_name="poly",
        filename_filter="*.shp",
    ) as alg:
      out_ds = alg.Output()
      self.assertGreater(out_ds.GetLayer(0).GetFeatureCount(), 0)

    with self.assertRaises(RuntimeError):
      gdal.Run(
          "vector",
          "index",
          input=ogr_dir,
          output_format="MEM",
          recursive=True,
          filename_filter="*.xxx",
      )


if __name__ == "__main__":
  absltest.main()
