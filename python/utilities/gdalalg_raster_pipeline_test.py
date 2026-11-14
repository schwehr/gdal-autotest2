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

"""Tests for `gdal raster pipeline`.

Ported from upstream autotest/utilities/test_gdalalg_raster_pipeline.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator, Sequence
import contextlib
import json
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_pipeline_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["pipeline"]


def _has_vrt_expression_dialect(dialect: str) -> bool:
  vrt = gdal.GetDriverByName("VRT")
  if vrt is None:
    return False
  dialects = vrt.GetMetadataItem("ExpressionDialects") or ""
  return dialect in dialects


@contextlib.contextmanager
def _capture_errors() -> Iterator[list[tuple[int, int, str]]]:
  errors: list[tuple[int, int, str]] = []

  def handler(err_level: int, err_no: int, err_msg: str) -> None:
    errors.append((err_level, err_no, err_msg))

  gdal.PushErrorHandler(handler)
  try:
    yield errors
  finally:
    gdal.PopErrorHandler()


class GdalalgRasterPipelineTest(gdaltest.TestCase, parameterized.TestCase):

  def _assert_gt_almost_equal(
      self, actual: Sequence[float], expected: Sequence[float]
  ) -> None:
    self.assertLen(actual, len(expected))
    for act_val, exp_val in zip(actual, expected):
      self.assertAlmostEqual(
          act_val, exp_val, delta=max(1e-8, abs(exp_val) * 1e-8)
      )

  def test_read_and_write(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_rw") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      last_pct = [0.0]

      def on_progress(pct: float, msg: str, user_data: object) -> bool:
        del msg, user_data
        last_pct[0] = pct
        return True

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              ["read", byte_tif, "!", "write", out_filename], on_progress
          )
      )
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      with self.assertRaisesRegex(
          RuntimeError, "can only be called once per instance"
      ):
        pipeline.ParseRunAndFinalize(
            ["read", byte_tif, "!", "write", out_filename], on_progress
        )

  def test_pipeline_arg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_arg") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "--pipeline",
              f"! read {byte_tif} | | write {out_filename} !",
          ])
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_as_api(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_api") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      pipeline["pipeline"] = f"read {byte_tif} ! write {out_filename}"
      self.assertTrue(pipeline.Run())
      ds = utilities_util.get_arg_dataset(pipeline, "output")
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      self.assertTrue(pipeline.Finalize())
      ds = None

      with gdal.OpenEx(out_filename) as out_ds:
        self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 4672)

  def test_input_through_api(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_in_api") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      pipeline["input"] = byte_tif
      pipeline["pipeline"] = f"read ! write {out_filename}"
      self.assertTrue(pipeline.Run())
      self.assertTrue(pipeline.Finalize())

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_input_through_api_run_twice(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_twice") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      pipeline["input"] = byte_tif
      pipeline["pipeline"] = f"read ! write {out_filename}"
      self.assertTrue(pipeline.Run())
      with self.assertRaisesRegex(
          RuntimeError,
          r"pipeline: Step nr 0 \(read\) has already an output dataset",
      ):
        pipeline.Run()

  def test_output_through_api(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_out_api") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      pipeline["output"] = out_filename
      pipeline["pipeline"] = f"read {byte_tif} ! write"
      self.assertTrue(pipeline.Run())
      self.assertTrue(pipeline.Finalize())

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_as_api_output(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    pipeline = _get_pipeline_alg()
    pipeline["pipeline"] = f"read {byte_tif}"
    self.assertTrue(pipeline.Run())
    ds = pipeline.Output()
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_mutually_exclusive_args(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with self.assertRaisesRegex(
        RuntimeError,
        "clip: Argument 'window' is mutually exclusive with 'bbox'",
    ):
      gdal.Run(
          "raster pipeline",
          input=byte_tif,
          output_format="MEM",
          output="",
          pipeline="read ! clip --bbox=1,2,3,4 --window=1,2,3,4 ! write",
      )

  def test_usage_as_json(self) -> None:
    pipeline = _get_pipeline_alg()
    parsed = json.loads(pipeline.GetUsageAsJSON())
    self.assertIn("pipeline_algorithms", parsed)

  def test_help_doc(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "--help-doc=main"]
    )
    self.assertEqual(code, 0)
    self.assertIn("Usage: gdal raster pipeline [OPTIONS] <PIPELINE>", out)
    self.assertIn("<PIPELINE> is of the form: ", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "--help-doc=edit"]
    )
    self.assertEqual(code, 0)
    self.assertIn("* edit [OPTIONS]", out)

    out, _, _ = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "--help-doc=unknown"]
    )
    self.assertIn("ERROR: unknown pipeline step 'unknown'", out)

  def test_quoted(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_quoted") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              [f"read {byte_tif} ! write {out_filename}"]
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_progress(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_filename = os.path.join(tmpdir, "out.tif")
      out, _, code = utilities_util.run_gdal_cli([
          "raster",
          "pipeline",
          "--progress",
          "read",
          byte_tif,
          "!",
          "write",
          out_filename,
      ])
      self.assertEqual(code, 0)
      self.assertTrue(
          out.startswith(
              "0...10...20...30...40...50...60...70...80...90...100 - done."
          )
      )
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_easter_egg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_filename = os.path.join(tmpdir, "out.tif")
      _, _, code = utilities_util.run_gdal_cli([
          "raster",
          "+gdal=pipeline",
          "+step",
          "+gdal=read",
          f"+input={byte_tif}",
          "+step",
          "+write",
          f"+output={out_filename}",
      ])
      self.assertEqual(code, 0)
      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_easter_egg_failed(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    _, err, code = utilities_util.run_gdal_cli([
        "raster",
        "+gdal=pipeline",
        "+step",
        "+gdal=read",
        f"+input={byte_tif}",
        "+step",
        "+gdal=unknown",
        "+step",
        "+write",
        "+output=/vsimem/out.tif",
    ])
    self.assertNotEqual(code, 0)
    self.assertIn("pipeline: unknown step name: unknown", err)

  def test_usage_as_json_cli(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "--json-usage"]
    )
    self.assertEqual(code, 0)
    self.assertIn("pipeline_algorithms", json.loads(out))

  def test_missing_at_run(self) -> None:
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError, "pipeline: 'pipeline' argument not set"
    ):
      pipeline.Run()

  def test_empty_args(self) -> None:
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError, "At least one step must be provided in a pipeline"
    ):
      pipeline.ParseRunAndFinalize([])

  def test_unknown_step(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError, "pipeline: unknown step name: unknown_step"
    ):
      pipeline.ParseRunAndFinalize([
          "read",
          byte_tif,
          "!",
          "unknown_step",
          "!",
          "write",
          "/vsimem/foo.tif",
      ])

  def test_read_read(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError, "pipeline: 'read' is only allowed as a first step"
    ):
      pipeline.ParseRunAndFinalize(["read", byte_tif, "!", "read", byte_tif])

  def test_write_write(self) -> None:
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError,
        "pipeline: First step should be 'read', 'calc', 'mosaic' or 'stack'",
    ):
      pipeline.ParseRunAndFinalize(
          ["write", "/vsimem/out.tif", "!", "write", "/vsimem/out.tif"]
      )

  def test_read_write_write(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError, "pipeline: Only last step can be 'write'"
    ):
      pipeline.ParseRunAndFinalize([
          "read",
          byte_tif,
          "!",
          "write",
          "/vsimem/out.tif",
          "!",
          "write",
          "/vsimem/out.tif",
      ])

  def test_read_read_write(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    pipeline = _get_pipeline_alg()
    with self.assertRaisesRegex(
        RuntimeError, "pipeline: Only first step can be 'read'"
    ):
      pipeline.ParseRunAndFinalize([
          "read",
          byte_tif,
          "!",
          "read",
          byte_tif,
          "!",
          "write",
          "/vsimem/out.tif",
      ])

  def test_invalid_step_during_parsing(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_bad_parse") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError, "write: Option '--invalid' is unknown"
      ):
        pipeline.ParseRunAndFinalize(
            ["read", byte_tif, "!", "write", "--invalid", out_filename]
        )

  def test_invalid_step_during_validation(self) -> None:
    with utilities_util.vsimem_dir("raster_pipeline_bad_val") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "read: Positional arguments starting at 'INPUT' have not been "
          "specified",
      ):
        pipeline.ParseRunAndFinalize(
            ["read", "!", "write", "--invalid", out_filename]
        )

  def test_write_options(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_gpkg_opts") as vsimem:
      out_filename = f"{vsimem}/out.gpkg"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              ["read", byte_tif, "!", "write", "--of=GPKG", out_filename]
          )
      )

      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        pipeline.ParseRunAndFinalize(
            ["read", byte_tif, "!", "write", out_filename]
        )

      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              ["read", byte_tif, "!", "write", "--overwrite", out_filename]
          )
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_write_co(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_gpkg_co") as vsimem:
      out_filename = f"{vsimem}/out.gpkg"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "write",
              out_filename,
              "--co",
              "ADD_GPKG_OGR_CONTENTS=NO",
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        with ds.ExecuteSQL(
            "SELECT * FROM sqlite_master WHERE name = 'gpkg_ogr_contents'"
        ) as lyr:
          self.assertEqual(lyr.GetFeatureCount(), 0)

  def test_reproject_invalid_src_crs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_bad_srccrs") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError, "reproject: Invalid value for 'src-crs' argument"
      ):
        pipeline.ParseRunAndFinalize([
            "read",
            byte_tif,
            "!",
            "reproject",
            "--src-crs=invalid",
            "--dst-crs=EPSG:4326",
            "!",
            "write",
            out_filename,
        ])

  def test_reproject_invalid_dst_crs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_bad_dstcrs") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError, "reproject: Invalid value for 'dst-crs' argument"
      ):
        pipeline.ParseRunAndFinalize([
            "read",
            byte_tif,
            "!",
            "reproject",
            "--dst-crs=invalid",
            "!",
            "write",
            out_filename,
        ])

  def test_reproject_invalid_resolution(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_bad_res") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "Value of argument 'resolution' is -1, but should be > 0",
      ):
        pipeline.ParseRunAndFinalize([
            "read",
            byte_tif,
            "!",
            "reproject",
            "--resolution=1,-1",
            "!",
            "write",
            out_filename,
        ])

  def test_reproject_no_args(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_reproj_noop") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "reproject",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "26711")
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_reproject_invalid_bbox(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_bad_bbox") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "Value of 'bbox' should be xmin,ymin,xmax,ymax with xmin <= xmax "
          "and ymin <= ymax",
      ):
        pipeline.ParseRunAndFinalize([
            "read",
            byte_tif,
            "!",
            "reproject",
            "--bbox=3,4,2,1",
            "!",
            "write",
            out_filename,
        ])

  def test_reproject_bbox_arg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_reproj_bbox") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "reproject",
              "--src-crs=EPSG:32611",
              "--dst-crs=EPSG:4326",
              "--bbox=-117.641,33.89,-117.628,33.9005",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "4326")
        self._assert_gt_almost_equal(
            ds.GetGeoTransform(),
            (
                -117.641,
                0.0005909090909093286,
                0.0,
                33.9005,
                0.0,
                -0.0005833333333333554,
            ),
        )
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4585)

  def test_reproject_almost_all_args(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_reproj_all") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "reproject",
              "--src-crs=EPSG:32611",
              "--dst-crs=EPSG:4326",
              "--resampling=bilinear",
              "--resolution=0.0005,0.0004",
              "--target-aligned-pixels",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "4326")
        self._assert_gt_almost_equal(
            ds.GetGeoTransform(),
            (-117.641, 0.0005, 0.0, 33.9008, 0.0, -0.0004),
        )
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 8515)

  def test_reproject_proj_string(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_proj_str") as vsimem:
      out_filename = f"{vsimem}/out.tif"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              byte_tif,
              "!",
              "reproject",
              "--src-crs=EPSG:32611",
              "--dst-crs",
              "+proj=laea +lon_0=147 +lat_0=-40 +datum=WGS84",
              "!",
              "write",
              "--overwrite",
              out_filename,
          ])
      )

      with gdal.OpenEx(out_filename) as ds:
        wkt = ds.GetSpatialRef().ExportToWkt(["FORMAT=WKT2"])
        self.assertIn("Lambert Azimuthal Equal Area", wkt)

  def test_too_many_steps_for_vrt_output(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_vrt_limit") as vsimem:
      out_filename = f"{vsimem}/out.vrt"
      pipeline = _get_pipeline_alg()
      with self.assertRaisesRegex(
          RuntimeError,
          "pipeline: VRT output is not supported when there are more than "
          "3 steps",
      ):
        pipeline.ParseRunAndFinalize([
            "read",
            byte_tif,
            "!",
            "reproject",
            "!",
            "reproject",
            "!",
            "write",
            "--overwrite",
            out_filename,
        ])

  @parameterized.parameters(
      ({},),
      ({"GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET": "YES"},),
  )
  def test_to_gdalg_step_non_natively_streamable(
      self, config_opts: dict[str, str]
  ) -> None:
    src_filename = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_pipeline_gdalg_nonstream") as vsimem:
      out_gdalg = f"{vsimem}/out.gdalg.json"
      with _capture_errors() as errors:
        gdal.Run(
            "raster",
            "pipeline",
            pipeline=f"read {src_filename} ! fill-nodata ! write {out_gdalg}",
        )
      self.assertTrue(any(lvl == gdal.CE_Warning for lvl, _, _ in errors))

      if utilities_util.has_driver("GDALG"):
        with gdal.config_options(config_opts):
          with gdal.Open(out_gdalg) as ds:
            self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

        bad_opts = {"CPL_TMPDIR": "/i_do/not/exist", **config_opts}
        with gdal.config_options(bad_opts):
          with self.assertRaises(RuntimeError):
            gdal.Open(out_gdalg)

  def test_help(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(["raster", "pipeline", "--help"])
    self.assertEqual(code, 0)
    self.assertTrue(
        out.startswith("Usage: gdal raster pipeline [OPTIONS] <PIPELINE>")
    )
    self.assertIn("* read [OPTIONS] <INPUT>", out)
    self.assertIn("* write [OPTIONS] <OUTPUT>", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "--progress", "--help"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(
        out.startswith("Usage: gdal raster pipeline [OPTIONS] <PIPELINE>")
    )
    self.assertIn("* read [OPTIONS] <INPUT>", out)
    self.assertIn("* write [OPTIONS] <OUTPUT>", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "read", "--help"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: read [OPTIONS] <INPUT>"))

    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "read", "foo.tif", "!", "select", "--help"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: select [OPTIONS] <BAND>"))

  def test_calc(self) -> None:
    if not _has_vrt_expression_dialect("muparser"):
      self.skipTest("muparser not available")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run(
        "raster",
        "pipeline",
        output_format="MEM",
        output="",
        pipeline=f"calc {byte_tif} --calc 255-X ! write",
    ) as alg:
      out_ds = alg.Output()
      self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 4563)

  def test_info(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run(
        "raster",
        "pipeline",
        pipeline=f"read {byte_tif} ! info",
    ) as alg:
      self.assertIn("bands", alg.Output())

  def test_info_executable(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out, _, code = utilities_util.run_gdal_cli(
        ["raster", "pipeline", "read", byte_tif, "!", "info"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Driver: GTiff/GeoTIFF"))


if __name__ == "__main__":
  absltest.main()
