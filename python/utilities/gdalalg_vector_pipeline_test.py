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

"""Tests for `gdal vector pipeline`.

Ported from upstream autotest/utilities/test_gdalalg_vector_pipeline.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import json
import os
import re

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_pipeline_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["pipeline"]


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
      gdal.FileFromMemBuffer(f"{dst_dir}/poly.{ext}", f_in.read())
  gdal.FileFromMemBuffer(f"{dst_dir}/poly.prj", _POLY_PRJ.encode("ascii"))
  return f"{dst_dir}/poly.shp"


class GdalalgVectorPipelineTest(gdaltest.TestCase):

  def test_read_and_write_and_run_once(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vpipe_rw") as tmpdir:
      out_filename = f"{tmpdir}/out.shp"
      last_pct = [0.0]

      def my_progress(
          pct: float, unused_msg: str, unused_user_data: object
      ) -> bool:
        last_pct[0] = pct
        return True

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              ["read", poly_shp, "!", "write", out_filename], my_progress
          )
      )
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      with self.assertRaisesRegex(
          RuntimeError, "can only be called once per instance"
      ):
        pipeline.ParseRunAndFinalize(
            ["read", poly_shp, "!", "write", out_filename], my_progress
        )

  def test_read_osm(self) -> None:
    if not utilities_util.has_driver("OSM"):
      self.skipTest("OSM driver not available")

    test_pbf = utilities_util.get_testdata_path("../ogr/data/osm/test.pbf")
    pipeline = _get_pipeline_alg()
    self.assertTrue(
        pipeline.ParseCommandLineArguments([
            "read",
            test_pbf,
            "!",
            "write",
            "--of=stream",
            "streamed_file",
        ])
    )
    self.assertTrue(pipeline.Run())

    out_ds = utilities_util.get_arg_dataset(pipeline, "output")
    self.assertTrue(out_ds.TestCapability(ogr.ODsCRandomLayerRead))
    self.assertEqual(out_ds.TestCapability("unknown"), 0)

    expected = []
    with gdal.OpenEx(test_pbf) as src_ds:
      while True:
        feat, _ = src_ds.GetNextFeature()
        if not feat:
          break
        expected.append(str(feat))

    got = []
    out_ds.ResetReading()
    while True:
      feat, _ = out_ds.GetNextFeature()
      if not feat:
        break
      got.append(str(feat))

    self.assertEqual(got, expected)

  def test_read_osm_subset_of_layers(self) -> None:
    if not utilities_util.has_driver("OSM"):
      self.skipTest("OSM driver not available")

    test_pbf = utilities_util.get_testdata_path("../ogr/data/osm/test.pbf")
    pipeline = _get_pipeline_alg()
    self.assertTrue(
        pipeline.ParseCommandLineArguments([
            "read",
            test_pbf,
            "--layer=points,multipolygons",
            "!",
            "write",
            "--of=stream",
            "streamed_file",
        ])
    )
    self.assertTrue(pipeline.Run())

    out_ds = utilities_util.get_arg_dataset(pipeline, "output")
    self.assertTrue(out_ds.TestCapability(ogr.ODsCRandomLayerRead))

    expected = []
    with gdal.OpenEx(test_pbf) as src_ds:
      while True:
        feat, lyr = src_ds.GetNextFeature()
        if not feat:
          break
        if lyr.GetName() in ("points", "multipolygons"):
          expected.append(str(feat))

    got = []
    out_ds.ResetReading()
    while True:
      feat, _ = out_ds.GetNextFeature()
      if not feat:
        break
      got.append(str(feat))

    self.assertEqual(got, expected)

  def test_pipeline_arg_and_api_modes(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vpipe_api") as tmpdir:
      out1 = f"{tmpdir}/out1.shp"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              ["--pipeline", f"! read {poly_shp} | | write {out1} !"]
          )
      )
      with gdal.OpenEx(out1) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out2 = f"{tmpdir}/out2.shp"
      pipeline = _get_pipeline_alg()
      pipeline["pipeline"] = f"read {poly_shp} ! write {out2}"
      self.assertTrue(pipeline.Run())
      ds = utilities_util.get_arg_dataset(pipeline, "output")
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
      self.assertTrue(pipeline.Finalize())
      ds = None
      with gdal.OpenEx(out2) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out3 = f"{tmpdir}/out3.shp"
      pipeline = _get_pipeline_alg()
      pipeline["input"] = gdal.OpenEx(poly_shp)
      pipeline["pipeline"] = f"read ! write {out3}"
      self.assertTrue(pipeline.Run())
      self.assertTrue(pipeline.Finalize())
      with gdal.OpenEx(out3) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out4 = f"{tmpdir}/out4.shp"
      pipeline = _get_pipeline_alg()
      pipeline["input"] = gdal.OpenEx(poly_shp)
      pipeline["pipeline"] = f"read ! write {out4}"
      self.assertTrue(pipeline.Run())
      with self.assertRaisesRegex(
          RuntimeError,
          r"pipeline: Step nr 0 \(read\) has already an output dataset",
      ):
        pipeline.Run()

      out5 = f"{tmpdir}/out5.shp"
      pipeline = _get_pipeline_alg()
      pipeline["output"] = out5
      pipeline["pipeline"] = f"read {poly_shp} ! write"
      self.assertTrue(pipeline.Run())
      self.assertTrue(pipeline.Finalize())
      with gdal.OpenEx(out5) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out6 = f"{tmpdir}/out6.shp"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([f"read {poly_shp} ! write {out6}"])
      )
      with gdal.OpenEx(out6) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_mutually_exclusive_args_and_json_usage(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with self.assertRaisesRegex(
        RuntimeError,
        "clip: Argument 'like' is mutually exclusive with 'bbox'",
    ):
      gdal.Run(
          "vector pipeline",
          input=poly_shp,
          output_format="MEM",
          output="",
          pipeline=f"read ! clip --bbox=1,2,3,4 --like={poly_shp} ! write",
      )

    pipeline = _get_pipeline_alg()
    parsed = json.loads(pipeline.GetUsageAsJSON())
    self.assertIn("pipeline_algorithms", parsed)

  def test_cli_help_doc_json_progress_and_easter_egg(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "--help-doc=main"]
    )
    self.assertEqual(code, 0)
    self.assertIn("Usage: gdal vector pipeline [OPTIONS] <PIPELINE>", out)
    self.assertIn("<PIPELINE> is of the form:", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "--help-doc=edit"]
    )
    self.assertEqual(code, 0)
    self.assertIn("* edit [OPTIONS]", out)

    out, _, _ = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "--help-doc=unknown"]
    )
    self.assertIn("ERROR: unknown pipeline step 'unknown'", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "--json-usage"]
    )
    self.assertEqual(code, 0)
    self.assertIn("pipeline_algorithms", json.loads(out))

    out, _, code = utilities_util.run_gdal_cli(["vector", "pipeline", "--help"])
    self.assertEqual(code, 0)
    self.assertTrue(
        out.startswith("Usage: gdal vector pipeline [OPTIONS] <PIPELINE>")
    )
    self.assertIn("* read [OPTIONS] <INPUT>", out)
    self.assertIn("* write [OPTIONS] <OUTPUT>", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "--progress", "--help"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(
        out.startswith("Usage: gdal vector pipeline [OPTIONS] <PIPELINE>")
    )

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "read", "--help"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: read [OPTIONS] <INPUT>"))

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "read", "foo.shp", "!", "select", "--help"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: select [OPTIONS] <FIELDS>"))

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_shp = os.path.join(tmpdir, "out.shp")
      out, _, code = utilities_util.run_gdal_cli([
          "vector",
          "pipeline",
          "--progress",
          "read",
          poly_shp,
          "!",
          "write",
          out_shp,
      ])
      self.assertEqual(code, 0)
      self.assertTrue(
          out.startswith(
              "0...10...20...30...40...50...60...70...80...90...100 - done."
          )
      )
      with gdal.OpenEx(out_shp) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      egg_shp = os.path.join(tmpdir, "egg.shp")
      _, _, code = utilities_util.run_gdal_cli([
          "vector",
          "+gdal=pipeline",
          "+step",
          "+gdal=read",
          f"+input={poly_shp}",
          "+step",
          "+write",
          f"+output={egg_shp}",
      ])
      self.assertEqual(code, 0)
      with gdal.OpenEx(egg_shp) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

    _, err, code = utilities_util.run_gdal_cli([
        "vector",
        "+gdal=pipeline",
        "+step",
        "+gdal=read",
        f"+input={poly_shp}",
        "+step",
        "+gdal=unknown",
        "+step",
        "+write",
        "+output=/vsimem/out.shp",
    ])
    self.assertNotEqual(code, 0)
    self.assertIn("pipeline: unknown step name: unknown", err)

  def test_pipeline_structure_and_parsing_errors(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    poly_dbf = utilities_util.get_testdata_path("../ogr/data/poly.dbf")

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: 'pipeline' argument not set"
    ):
      _get_pipeline_alg().Run()

    with self.assertRaisesRegex(
        RuntimeError,
        "pipeline: At least one step must be provided in a pipeline",
    ):
      _get_pipeline_alg().ParseRunAndFinalize([])

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: unknown step name: unknown_step"
    ):
      _get_pipeline_alg().ParseRunAndFinalize([
          "read",
          poly_shp,
          "!",
          "unknown_step",
          "!",
          "write",
          "/vsimem/foo.shp",
      ])

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: 'read' is only allowed as a first step"
    ):
      _get_pipeline_alg().ParseRunAndFinalize(
          ["read", poly_shp, "!", "read", poly_shp]
      )

    with self.assertRaisesRegex(
        RuntimeError,
        f"read: Positional values starting at '{re.escape(poly_dbf)}'"
        " are not expected.",
    ):
      _get_pipeline_alg().ParseRunAndFinalize(
          ["read", poly_shp, poly_dbf, "!", "write", "/vsimem/poly.shp"]
      )

    pipeline = _get_pipeline_alg()
    pipeline["input"] = [poly_shp, poly_dbf]
    pipeline["output"] = "/vsimem/poly.shp"
    pipeline["pipeline"] = "read ! write"
    with self.assertRaisesRegex(
        RuntimeError,
        "read: 2 values have been specified for argument 'input', whereas"
        " exactly 1 was expected.",
    ):
      pipeline.Run()

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: First step should be 'read'"
    ):
      _get_pipeline_alg().ParseRunAndFinalize(
          ["write", "/vsimem/poly.shp", "!", "write", "/vsimem/poly.shp"]
      )

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: Only last step can be 'write'"
    ):
      _get_pipeline_alg().ParseRunAndFinalize([
          "read",
          poly_shp,
          "!",
          "write",
          "/vsimem/poly.shp",
          "!",
          "write",
          "/vsimem/poly.shp",
      ])

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: Only first step can be 'read'"
    ):
      _get_pipeline_alg().ParseRunAndFinalize([
          "read",
          poly_shp,
          "!",
          "read",
          poly_shp,
          "!",
          "write",
          "/vsimem/poly.shp",
      ])

    with utilities_util.vsimem_dir("vpipe_parse_err") as tmpdir:
      out_filename = f"{tmpdir}/out.shp"
      with self.assertRaisesRegex(
          RuntimeError, "write: Option '--invalid' is unknown"
      ):
        _get_pipeline_alg().ParseRunAndFinalize(
            ["read", poly_shp, "!", "write", "--invalid", out_filename]
        )

      with self.assertRaisesRegex(
          RuntimeError,
          "read: Positional arguments starting at 'INPUT' have not been"
          " specified",
      ):
        _get_pipeline_alg().ParseRunAndFinalize(
            ["read", "!", "write", "--invalid", out_filename]
        )

  def test_read_layername(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vpipe_layername") as tmpdir:
      out_filename = f"{tmpdir}/out.shp"
      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              "--layer",
              "poly",
              poly_shp,
              "!",
              "write",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out_invalid = f"{tmpdir}/out_invalid.shp"
      with self.assertRaisesRegex(
          RuntimeError, "read: Cannot find source layer 'invalid'"
      ):
        _get_pipeline_alg().ParseRunAndFinalize([
            "read",
            "--layer",
            "invalid",
            poly_shp,
            "!",
            "write",
            out_invalid,
        ])

  def test_write_options_and_co_lco_gpkg(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vpipe_gpkg_opts") as tmpdir:
      out_filename = f"{tmpdir}/out.gpkg"

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize(
              ["read", poly_shp, "!", "write", "--of=GPKG", out_filename]
          )
      )

      with self.assertRaisesRegex(RuntimeError, "already exists"):
        _get_pipeline_alg().ParseRunAndFinalize(
            ["read", poly_shp, "!", "write", out_filename]
        )

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize(
              ["read", poly_shp, "!", "write", "--append", out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 20)

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "write",
              "--update",
              "--output-layer=layer2",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer("poly").GetFeatureCount(), 20)
        self.assertEqual(ds.GetLayer("layer2").GetFeatureCount(), 10)

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "write",
              "--overwrite-layer",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayer("poly").GetFeatureCount(), 10)
        self.assertEqual(ds.GetLayer("layer2").GetFeatureCount(), 10)

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize(
              ["read", poly_shp, "!", "write", "--overwrite", out_filename]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      dsco_gpkg = f"{tmpdir}/dsco.gpkg"
      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "write",
              dsco_gpkg,
              "--co",
              "ADD_GPKG_OGR_CONTENTS=NO",
          ])
      )
      with gdal.OpenEx(dsco_gpkg) as ds:
        with ds.ExecuteSQL(
            "SELECT * FROM sqlite_master WHERE name = 'gpkg_ogr_contents'"
        ) as lyr:
          self.assertEqual(lyr.GetFeatureCount(), 0)

      lco_gpkg = f"{tmpdir}/lco.gpkg"
      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "write",
              lco_gpkg,
              "--lco",
              "FID=my_fid",
          ])
      )
      with gdal.OpenEx(lco_gpkg) as ds:
        self.assertEqual(ds.GetLayer(0).GetFIDColumn(), "my_fid")

  def test_filter_and_bbox(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with utilities_util.vsimem_dir("vpipe_filter") as tmpdir:
      out_noop = f"{tmpdir}/out_noop.shp"
      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize(
              ["read", poly_shp, "!", "filter", "!", "write", out_noop]
          )
      )
      with gdal.OpenEx(out_noop) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out_bbox = f"{tmpdir}/out_bbox.shp"
      for bad_bbox in ("--bbox=2,49,1,50", "--bbox=2,49,3,48"):
        with self.assertRaisesRegex(
            RuntimeError,
            "Value of 'bbox' should be xmin,ymin,xmax,ymax with xmin <= xmax"
            " and ymin <= ymax",
        ):
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "filter",
              bad_bbox,
              "!",
              "write",
              out_bbox,
          ])

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "filter",
              "--bbox=0,0,0,0",
              "!",
              "write",
              out_bbox,
          ])
      )
      with gdal.OpenEx(out_bbox) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "filter",
              "--bbox=479867,4762909,479868,4762910",
              "!",
              "write",
              "--overwrite",
              out_bbox,
          ])
      )
      with gdal.OpenEx(out_bbox) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_reproject(self) -> None:
    with utilities_util.vsimem_dir("vpipe_reproj") as tmpdir:
      poly_shp = _copy_poly_with_prj(tmpdir)
      out_filename = f"{tmpdir}/out.shp"

      with self.assertRaisesRegex(
          RuntimeError,
          "reproject: Required argument 'dst-crs' has not been specified",
      ):
        _get_pipeline_alg().ParseRunAndFinalize(
            ["read", poly_shp, "!", "reproject", "!", "write", out_filename]
        )

      with self.assertRaisesRegex(
          RuntimeError, "reproject: Invalid value for 'src-crs' argument"
      ):
        _get_pipeline_alg().ParseRunAndFinalize([
            "read",
            poly_shp,
            "!",
            "reproject",
            "--src-crs=invalid",
            "--dst-crs=EPSG:4326",
            "!",
            "write",
            out_filename,
        ])

      with self.assertRaisesRegex(
          RuntimeError, "reproject: Invalid value for 'dst-crs' argument"
      ):
        _get_pipeline_alg().ParseRunAndFinalize([
            "read",
            poly_shp,
            "!",
            "reproject",
            "--dst-crs=invalid",
            "!",
            "write",
            out_filename,
        ])

      pipeline = _get_pipeline_alg()
      mem_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
      mem_ds.CreateLayer("layer")
      pipeline["input"] = mem_ds
      pipeline["pipeline"] = (
          f"read ! reproject --dst-crs=EPSG:4326 ! write {out_filename}"
      )
      with self.assertRaisesRegex(
          RuntimeError,
          "reproject: Layer 'layer' has no spatial reference system",
      ):
        pipeline.Run()

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "reproject",
              "--dst-crs=EPSG:4326",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(
            ds.GetLayer(0).GetSpatialRef().GetAuthorityCode(None), "4326"
        )
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "reproject",
              "--src-crs=EPSG:32631",
              "--dst-crs=EPSG:4326",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), "4326")
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        env = feat.GetGeometryRef().GetEnvelope()
        expected_env = (
            2.750130423614134,
            2.759262932833617,
            43.0361359661472,
            43.0429263707128,
        )
        for got_v, exp_v in zip(env, expected_env):
          self.assertAlmostEqual(got_v, exp_v, places=6)

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              poly_shp,
              "!",
              "reproject",
              "--src-crs=EPSG:32631",
              "--dst-crs",
              "+proj=laea +lon_0=147 +lat_0=-40 +datum=WGS84",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        wkt2 = ds.GetLayer(0).GetSpatialRef().ExportToWkt(["FORMAT=WKT2"])
        self.assertIn("Lambert Azimuthal Equal Area", wkt2)

  def test_geom_and_skip_errors(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    alg = _get_pipeline_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    with self.assertRaisesRegex(
        RuntimeError,
        "pipeline: 'unknown-subalgorithm' is a unknown sub-algorithm of 'geom'",
    ):
      alg.ParseCommandLineArguments(
          ["read", "!", "geom", "unknown-subalgorithm", "!", "write"]
      )

    src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
    src_lyr = src_ds.CreateLayer("the_layer")
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT (3 0)"))
    src_lyr.CreateFeature(feat)

    alg = _get_pipeline_alg()
    alg["input"] = src_ds
    alg["output"] = ""
    alg["output-format"] = "stream"
    self.assertTrue(
        alg.ParseCommandLineArguments([
            "read",
            "!",
            "geom",
            "set-type",
            "--geometry-type=POINTZ",
            "!",
            "write",
        ])
    )
    self.assertTrue(alg.Run())
    out_ds = utilities_util.get_arg_dataset(alg, "output")
    out_lyr = out_ds.GetLayer(0)
    self.assertEqual(out_lyr.GetGeomType(), ogr.wkbPoint25D)
    out_feat = out_lyr.GetNextFeature()
    self.assertEqual(
        out_feat.GetGeometryRef().ExportToIsoWkt(), "POINT Z (3 0 0)"
    )

    with utilities_util.vsimem_dir("vpipe_skip_err") as tmpdir:
      mix_ds = gdal.GetDriverByName("MEM").CreateVector("")
      mix_lyr = mix_ds.CreateLayer("test")
      for wkt in ("POINT(1 2)", "LINESTRING(1 2, 3 4)", "POINT(3 4)"):
        f = ogr.Feature(mix_lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        mix_lyr.CreateFeature(f)

      alg = _get_pipeline_alg()
      alg["input"] = mix_ds
      alg["output"] = f"{tmpdir}/out.shp"
      self.assertTrue(
          alg.ParseCommandLineArguments(["read", "!", "write", "--skip-errors"])
      )
      self.assertTrue(alg.Run())
      out_ds2 = utilities_util.get_arg_dataset(alg, "output")
      self.assertEqual(out_ds2.GetLayer(0).GetFeatureCount(), 2)

  def test_info_and_limit(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with gdal.Run(
        "vector", "pipeline", pipeline=f"read {poly_shp} ! info"
    ) as alg:
      self.assertIn("layers", alg.Output())

    out, _, code = utilities_util.run_gdal_cli(
        ["vector", "pipeline", "read", poly_shp, "!", "info"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith(f"INFO: Open of `{poly_shp}'"))

    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with utilities_util.vsimem_dir("vpipe_limit") as tmpdir:
      src_filename = f"{tmpdir}/src.gpkg"
      dst_filename = f"{tmpdir}/dst.gpkg"

      src_ds = gdal.GetDriverByName("GPKG").CreateVector(src_filename)
      for lyr_name in ("layer1", "layer2"):
        lyr = src_ds.CreateLayer(lyr_name)
        for _ in range(5):
          lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
      src_ds = None

      self.assertTrue(
          _get_pipeline_alg().ParseRunAndFinalize([
              "read",
              src_filename,
              "!",
              "limit",
              "3",
              "!",
              "write",
              dst_filename,
          ])
      )
      with gdal.OpenEx(dst_filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 3)
        self.assertEqual(ds.GetLayer(1).GetFeatureCount(), 3)


if __name__ == "__main__":
  absltest.main()
