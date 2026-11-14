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

"""Tests for `gdal pipeline`.

Ported from upstream autotest/utilities/test_gdalalg_pipeline.py.

Upstream copyright:
  Copyright (c) 2024, 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib
import json
import os

from absl.testing import absltest
from osgeo import gdal
from osgeo import ogr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_pipeline_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["pipeline"]


@contextlib.contextmanager
def _expect_warning(match: str = "") -> Iterator[None]:
  events: list[tuple[int, str]] = []

  def _handler(err_class: int, err_no: int, msg: str) -> None:
    del err_no
    events.append((err_class, msg))

  gdal.PushErrorHandler(_handler)
  try:
    yield
  finally:
    gdal.PopErrorHandler()

  if not any(lvl == gdal.CE_Warning and match in msg for lvl, msg in events):
    raise AssertionError(
        f"Expected CE_Warning matching {match!r}, got {events!r}"
    )


def _create_points_ds(geom_3d: bool) -> gdal.Dataset:
  src_ds = gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)
  src_lyr = src_ds.CreateLayer("test")
  src_lyr.CreateField(ogr.FieldDefn("z", ogr.OFTReal))
  for x, y, z in (
      (100, 1000, 500),
      (110, 1000, 400),
      (100, 1010, 300),
      (110, 1010, 600),
      (109, 1009, 610),
  ):
    feat = ogr.Feature(src_lyr.GetLayerDefn())
    if geom_3d:
      feat.SetGeometry(ogr.CreateGeometryFromWkt(f"POINT Z({x} {y} {z})"))
    else:
      feat["z"] = z
      feat.SetGeometry(ogr.CreateGeometryFromWkt(f"POINT({x} {y})"))
    src_lyr.CreateFeature(feat)
  return src_ds


class GdalalgPipelineTest(gdaltest.TestCase):

  def test_read_and_write_vector(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    last_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: object) -> bool:
      del msg, user_data
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("pipeline_rw_vec") as tmp:
      out_filename = f"{tmp}/out.shp"
      pipeline = _get_pipeline_alg()
      self.assertTrue(
          pipeline.ParseRunAndFinalize(
              [
                  "read",
                  poly_shp,
                  "!",
                  "edit",
                  "--layer-metadata",
                  "FOO=BAR",
                  "!",
                  "write",
                  out_filename,
              ],
              my_progress,
          )
      )
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_read_and_write_raster(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    last_pct = [0.0]

    def my_progress(pct: float, msg: str, user_data: object) -> bool:
      del msg, user_data
      last_pct[0] = pct
      return True

    with utilities_util.vsimem_dir("pipeline_rw_ras") as tmp:
      out_filename = f"{tmp}/out.tif"
      pipeline = _get_pipeline_alg()
      with _expect_warning():
        self.assertTrue(
            pipeline.ParseRunAndFinalize(
                [
                    "read",
                    byte_tif,
                    "!",
                    "edit",
                    "--gcp=1,2,3,4",
                    "!",
                    "write",
                    out_filename,
                ],
                my_progress,
            )
        )
      self.assertEqual(last_pct[0], 1.0)

      with gdal.OpenEx(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_read_and_write_from_object(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    src_vec = gdal.OpenEx(poly_shp, gdal.OF_VECTOR)
    with gdal.Run(
        "pipeline",
        input=src_vec,
        output_format="MEM",
        output="",
        pipeline="read ! write",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    src_ras = gdal.Open(byte_tif)
    with gdal.Run(
        "pipeline",
        input=src_ras,
        output_format="MEM",
        output="",
        pipeline="read",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

  def test_read_vector_write_raster(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    with (
        gdal.Run(
            "pipeline",
            pipeline=(
                f"read {poly_shp} ! rasterize --size 256,256 !"
                " write --output-format stream streamed_dataset"
            ),
        ) as alg,
        gdal.Run(
            "vector",
            "rasterize",
            input=poly_shp,
            size=[256, 256],
            output_format="MEM",
            output="",
        ) as alg2,
    ):
      ds = alg.Output()
      ds2 = alg2.Output()
      self.assertEqual(
          ds.GetRasterBand(1).Checksum(),
          ds2.GetRasterBand(1).Checksum(),
      )

  def test_errors(self) -> None:
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")

    alg = _get_pipeline_alg()
    alg.ParseCommandLineArguments([
        "read",
        poly_shp,
        "!",
        "write",
        "--output-format=stream",
        "streamed_dataset",
    ])
    with self.assertRaisesRegex(
        RuntimeError, "can only be called once per instance"
    ):
      alg.ParseCommandLineArguments([
          "read",
          poly_shp,
          "!",
          "write",
          "--output-format=stream",
          "streamed_dataset",
      ])

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: unknown step name: foo"
    ):
      gdal.Run("pipeline", pipeline="foo")

    with self.assertRaisesRegex(
        RuntimeError, "read: Option '--bar' is unknown"
    ):
      gdal.Run("pipeline", pipeline=f"read {byte_tif} --bar ! write")

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: Only first step can be 'read'"
    ):
      gdal.Run("pipeline", pipeline=f"read {byte_tif} ! read ! write")

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: Only last step can be 'write'"
    ):
      gdal.Run("pipeline", pipeline=f"read {byte_tif} ! write ! write")

    with self.assertRaisesRegex(
        RuntimeError, "pipeline: First step should be 'read',"
    ):
      gdal.Run("pipeline", pipeline="write ! reproject")

    with self.assertRaisesRegex(
        RuntimeError,
        "pipeline: Step 'buffer' expects a vector input dataset, but previous"
        " step 'read' generates a raster output dataset",
    ):
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! buffer --distance=1 !"
              " write --output-format=stream streamed_dataset"
          ),
      )

    with self.assertRaisesRegex(
        RuntimeError,
        "pipeline: Step 'hillshade' expects a raster input dataset, but"
        " previous step 'read' generates a vector output dataset",
    ):
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {poly_shp} ! hillshade !"
              " write --output-format=stream streamed_dataset"
          ),
      )

    with self.assertRaisesRegex(
        RuntimeError,
        "buffer: Positional arguments starting at 'DISTANCE' have not been"
        " specified",
    ):
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {poly_shp} ! buffer !"
              " write --output-format=stream streamed_dataset"
          ),
      )

    with self.assertRaisesRegex(
        RuntimeError, "edit: Option '--gcp' is unknown"
    ):
      gdal.Run(
          "pipeline",
          pipeline=(
              f"read {poly_shp} ! edit --gcp=1,2,3,4 !"
              " write --output-format=stream streamed_dataset"
          ),
      )

    with self.assertRaisesRegex(
        RuntimeError,
        "clip: Argument 'window' is mutually exclusive with 'bbox'",
    ):
      gdal.Run(
          "pipeline",
          input=byte_tif,
          output_format="MEM",
          output="",
          pipeline="read ! clip --bbox=1,2,3,4 --window=1,2,3,4 ! write",
      )

  def test_command_line_and_help(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      _, err, code = utilities_util.run_gdal_cli(["pipeline", "read", byte_tif])
      self.assertEqual(code, 1)
      self.assertIn("pipeline: At least 2 steps must be provided", err)

      out1 = os.path.join(tmpdir, "out.tif")
      out, _, code = utilities_util.run_gdal_cli(
          ["pipeline", "--progress", "read", byte_tif, "!", "write", out1]
      )
      self.assertEqual(code, 0)
      self.assertIn(
          "0...10...20...30...40...50...60...70...80...90...100 - done", out
      )

      out2 = os.path.join(tmpdir, "out2.tif")
      out, _, code = utilities_util.run_gdal_cli(
          ["pipeline", "--progress", f"read {byte_tif} ! write {out2}"]
      )
      self.assertEqual(code, 0)
      self.assertIn(
          "0...10...20...30...40...50...60...70...80...90...100 - done", out
      )

    _, err, code = utilities_util.run_gdal_cli([
        "pipeline",
        "read",
        byte_tif,
        "!",
        "reproject",
        "--resampling=?",
    ])
    self.assertEqual(code, 1)
    self.assertIn(
        "Potential values for argument 'resampling' are:\n"
        "- nearest\n- bilinear",
        err.replace("\r\n", "\n"),
    )

    out, _, code = utilities_util.run_gdal_cli(["pipeline", "--help"])
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Usage: gdal pipeline [OPTIONS] <PIPELINE>"))
    self.assertIn("read", out)
    self.assertIn("concat", out)
    self.assertIn("rasterize", out)

    out, _, code = utilities_util.run_gdal_cli(["pipeline", "--help-doc=main"])
    self.assertEqual(code, 0)
    self.assertIn("Usage: gdal pipeline [OPTIONS] <PIPELINE>", out)
    self.assertIn("<PIPELINE> is of the form:", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["pipeline", "--help-doc=edit-raster"]
    )
    self.assertEqual(code, 0)
    self.assertIn("* edit [OPTIONS]", out)

    out, _, _ = utilities_util.run_gdal_cli(["pipeline", "--help-doc=unknown"])
    self.assertIn("ERROR: unknown pipeline step 'unknown'", out)

  def test_raster_and_vector_output_to_gdalg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_raster_gdalg = os.path.join(tmpdir, "out_raster.gdalg.json")
      _, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          "read",
          byte_tif,
          "!",
          "write",
          out_raster_gdalg,
      ])
      self.assertEqual(code, 0)
      with open(out_raster_gdalg, "rb") as f:
        parsed = json.load(f)
      self.assertIn("gdal_version", parsed)
      del parsed["gdal_version"]
      self.assertEqual(
          parsed,
          {
              "command_line": f"gdal pipeline read --input {byte_tif}",
              "type": "gdal_streamed_alg",
          },
      )
      if utilities_util.has_driver("GDALG"):
        with gdal.Open(out_raster_gdalg) as ds:
          self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

      out_vector_gdalg = os.path.join(tmpdir, "out_vector.gdalg.json")
      _, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          "read",
          poly_shp,
          "!",
          "write",
          out_vector_gdalg,
      ])
      self.assertEqual(code, 0)
      with open(out_vector_gdalg, "rb") as f:
        parsed_vec = json.load(f)
      self.assertIn("gdal_version", parsed_vec)
      del parsed_vec["gdal_version"]
      self.assertEqual(
          parsed_vec,
          {
              "command_line": f"gdal pipeline read --input {poly_shp}",
              "type": "gdal_streamed_alg",
          },
      )
      if utilities_util.has_driver("GDALG"):
        with gdal.OpenEx(out_vector_gdalg, gdal.OF_VECTOR) as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_steps_grid_footprint_polygonize_contour_calc_info(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    src_ds = _create_points_ds(geom_3d=True)
    with gdal.Run(
        "pipeline",
        input=src_ds,
        output_format="MEM",
        output="",
        pipeline="read ! grid average ! write",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 35730)

    with gdal.Run(
        "pipeline",
        input=byte_tif,
        output_format="MEM",
        output="",
        pipeline="read ! footprint ! write",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

    with gdal.Run(
        "pipeline",
        input=byte_tif,
        output_format="MEM",
        output="",
        pipeline="read ! polygonize ! write",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 281)

    with gdal.Run(
        "pipeline",
        input=byte_tif,
        output_format="MEM",
        output="",
        pipeline="read ! contour --interval 10 ! write",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 218)

    with gdal.Run("pipeline", pipeline=f"read {byte_tif} ! info") as alg:
      self.assertIn("bands", alg.Output())

    with gdal.Run("pipeline", pipeline=f"read {poly_shp} ! info") as alg:
      self.assertIn("layers", alg.Output())

    out, _, code = utilities_util.run_gdal_cli(
        ["pipeline", "--pipeline", f"read {byte_tif} ! info"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith("Driver: GTiff/GeoTIFF"))

    out, _, code = utilities_util.run_gdal_cli(
        ["pipeline", f"--pipeline=read {poly_shp} ! info"]
    )
    self.assertEqual(code, 0)
    self.assertTrue(out.startswith(f"INFO: Open of `{poly_shp}'"))

  def test_calc_step(self) -> None:
    vrt_drv = gdal.GetDriverByName("VRT")
    dialects = (
        vrt_drv.GetMetadataItem("ExpressionDialects") if vrt_drv else None
    )
    if not dialects or "muparser" not in dialects:
      self.skipTest("muparser not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gdal.Run(
        "pipeline",
        output_format="MEM",
        output="",
        pipeline=f"calc {byte_tif} --calc 255-X ! write",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 4563)

  def test_run_existing(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    rgbsmall_tif = utilities_util.get_testdata_path(
        "../gcore/data/rgbsmall.tif"
    )
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      pipeline_filename = os.path.join(tmpdir, "pipeline.gdalg.json")
      output_filename = os.path.join(tmpdir, "out.tif")

      def write_spec(spec: dict[str, object]) -> None:
        with open(pipeline_filename, "w", encoding="utf-8") as f:
          json.dump(spec, f)

      write_spec({
          "type": "gdal_streamed_alg",
          "relative_paths_relative_to_this_file": False,
          "command_line": (
              f"gdal pipeline read {byte_tif} ! edit --nodata=5"
              " --metadata=KEY=VALUE"
          ),
      })

      with gdal.Run(
          "pipeline",
          pipeline=pipeline_filename,
          output="",
          output_format="MEM",
          arguments={"edit.metadata": "FOO=BAR"},
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.GetDriver().ShortName, "MEM")
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 5)
        self.assertEqual(ds.GetMetadataItem("FOO"), "BAR")

      out, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          pipeline_filename,
          "--write.output",
          output_filename,
          "--quiet",
      ])
      self.assertEqual(code, 0)
      self.assertEqual(out, "")
      with gdal.Open(output_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetMetadataItem("KEY"), "VALUE")

      _, err, code = utilities_util.run_gdal_cli([
          "pipeline",
          pipeline_filename,
          "--output",
          output_filename,
          "--quiet",
      ])
      self.assertEqual(code, 1)
      self.assertIn("--overwrite", err)
      gdal.Unlink(output_filename)

      out, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          f"--pipeline={pipeline_filename}",
          "--output",
          output_filename,
          "--quiet",
      ])
      self.assertEqual(code, 0)
      self.assertEqual(out, "")
      with gdal.Open(output_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
        self.assertEqual(ds.GetMetadataItem("KEY"), "VALUE")
      gdal.Unlink(output_filename)

      out, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          "--pipeline",
          pipeline_filename,
          "--input",
          rgbsmall_tif,
          "--output",
          output_filename,
          "--metadata",
          "FOO=BAR",
          "--quiet",
      ])
      self.assertEqual(code, 0)
      self.assertEqual(out, "")
      with gdal.Open(output_filename) as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.GetMetadataItem("FOO"), "BAR")
      gdal.Unlink(output_filename)

      write_spec({
          "type": "gdal_streamed_alg",
          "relative_paths_relative_to_this_file": False,
          "command_line": (
              f"gdal pipeline read {byte_tif} ! edit --metadata KEY=VALUE"
          ),
      })
      out, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          "--pipeline",
          pipeline_filename,
          "--input",
          rgbsmall_tif,
          "--output",
          output_filename,
          "--metadata=FOO=BAR",
          "--quiet",
      ])
      self.assertEqual(code, 0)
      self.assertEqual(out, "")
      with gdal.Open(output_filename) as ds:
        self.assertEqual(ds.RasterCount, 3)
        self.assertEqual(ds.GetMetadataItem("FOO"), "BAR")
      gdal.Unlink(output_filename)

      for args, expected_err in (
          (
              ["pipeline", pipeline_filename],
              (
                  "write: Positional arguments starting at 'OUTPUT' have not"
                  " been specified"
              ),
          ),
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--foo.bar=baz",
              ],
              (
                  "Argument 'foo.bar' refers to a non-existing 'foo' step in"
                  " the pipeline"
              ),
          ),
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--read.bar=baz",
              ],
              "read: Option '--bar' is unknown",
          ),
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--foo=bar",
              ],
              "pipeline: No step in the pipeline has an argument named 'foo'",
          ),
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--foo.bar.baz=baw",
              ],
              (
                  "Invalid argument name 'foo.bar.baz'. It should of the form"
                  " <algorithm-name>.<argument-name>"
              ),
          ),
      ):
        _, err, _ = utilities_util.run_gdal_cli(args)
        self.assertIn(expected_err, err)

      write_spec({
          "type": "gdal_streamed_alg",
          "relative_paths_relative_to_this_file": False,
          "command_line": (
              f"gdal pipeline read {byte_tif} ! edit --metadata=KEY=VALUE !"
              " edit ! write --output-format=stream streamed_dataset"
          ),
      })
      for args, expected_err in (
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--metadata=foo=bar",
              ],
              (
                  "Ambiguous argument name 'metadata', because it is valid for"
                  " several steps in the pipeline. It should be specified with"
                  " the form <algorithm-name>.<argument-name>"
              ),
          ),
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--edit.metadata=foo=bar",
              ],
              (
                  "pipeline: Argument 'edit.metadata' is ambiguous as there are"
                  " several 'edit' steps in the pipeline"
              ),
          ),
          (
              [
                  "pipeline",
                  pipeline_filename,
                  "--output",
                  output_filename,
                  "--edit[2].metadata=foo=bar",
              ],
              (
                  "pipeline: Argument 'edit[2].metadata' refers to a"
                  " non-existing 'edit[2]' step in the pipeline"
              ),
          ),
      ):
        _, err, _ = utilities_util.run_gdal_cli(args)
        self.assertIn(expected_err, err)

      out, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          pipeline_filename,
          "--output",
          output_filename,
          "--edit[1].metadata=foo=bar",
          "--quiet",
      ])
      self.assertEqual(code, 0)
      self.assertEqual(out, "")
      with gdal.Open(output_filename) as ds:
        self.assertEqual(ds.GetMetadataItem("KEY"), "VALUE")
        self.assertEqual(ds.GetMetadataItem("foo"), "bar")
      gdal.Unlink(output_filename)

      write_spec({
          "type": "gdal_streamed_alg",
          "relative_paths_relative_to_this_file": False,
          "command_line": (
              f"gdal pipeline read {poly_shp} ! edit"
              " --geometry-type MULTIPOLYGON --metadata=KEY=VALUE"
          ),
      })
      with gdal.Run(
          "pipeline",
          pipeline=pipeline_filename,
          output="",
          output_format="MEM",
          arguments={"edit.metadata": "FOO=BAR"},
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.GetDriver().ShortName, "MEM")
        self.assertEqual(ds.GetLayerCount(), 1)
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbMultiPolygon)
        self.assertEqual(ds.GetMetadataItem("FOO"), "BAR")

      with open(pipeline_filename, "wb") as f:
        f.write(b"corrupted")
      _, err, _ = utilities_util.run_gdal_cli([
          "pipeline",
          pipeline_filename,
          "--output",
          output_filename,
          "--edit[2].metadata=foo=bar",
      ])
      self.assertIn("JSON parsing error", err)

      write_spec({"type": "gdal_streamed_alg"})
      _, err, _ = utilities_util.run_gdal_cli([
          "pipeline",
          pipeline_filename,
          "--output",
          output_filename,
          "--edit[2].metadata=foo=bar",
      ])
      self.assertIn("pipeline: command_line missing in", err)

      for write_step in (
          "write --output-format=stream streamed_dataset",
          "write --output-format stream --output streamed_dataset",
      ):
        write_spec({
            "type": "gdal_streamed_alg",
            "relative_paths_relative_to_this_file": False,
            "command_line": (
                "gdal pipeline read a b c ! edit --metadata=KEY=VALUE ! edit !"
                f" {write_step}"
            ),
        })
        _, err, _ = utilities_util.run_gdal_cli([
            "pipeline",
            pipeline_filename,
            "--output",
            output_filename,
            "--input=/i/do_not/exist",
        ])
        self.assertIn("/i/do_not/exist", err)

  def test_existing_completion(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      pipeline_filename = os.path.join(tmpdir, "pipeline.gdalg.json")

      def write_spec(command_line: str) -> None:
        with open(pipeline_filename, "w", encoding="utf-8") as f:
          json.dump(
              {
                  "type": "gdal_streamed_alg",
                  "relative_paths_relative_to_this_file": False,
                  "command_line": command_line,
              },
              f,
          )

      write_spec(
          f"gdal pipeline read {byte_tif} ! edit --nodata=5"
          " --metadata=KEY=VALUE"
      )
      out, _, _ = utilities_util.run_gdal_cli(
          ["completion", "gdal", "pipeline", pipeline_filename, "-"]
      )
      tokens = out.split(" ")
      for expected in (
          "--input-format=",
          "--open-option=",
          "--input=",
          "--edit.crs=",
          "--edit.bbox=",
          "--edit.nodata=",
          "--edit.metadata=",
          "--edit.unset-metadata=",
          "--edit.gcp=",
          "--output-format=",
          "--output=",
          "--creation-option=",
          "--overwrite",
      ):
        self.assertIn(expected, tokens)
      self.assertNotIn("--input-layer=", tokens)

      out, _, _ = utilities_util.run_gdal_cli([
          "completion",
          "gdal",
          "pipeline",
          pipeline_filename,
          "--input-format=",
      ])
      self.assertIn("MEM", out.split(" "))

      out, _, _ = utilities_util.run_gdal_cli([
          "completion",
          "gdal",
          "pipeline",
          pipeline_filename,
          "--output=/vsimem/out.tif",
          "--creation-option=",
      ])
      self.assertIn("COMPRESS=", out.split(" "))

      write_spec(f"gdal pipeline read {poly_shp} ! edit")
      out, _, _ = utilities_util.run_gdal_cli(
          ["completion", "gdal", "pipeline", pipeline_filename, "-"]
      )
      tokens = out.split(" ")
      for expected in (
          "--input-format=",
          "--open-option=",
          "--input=",
          "--input-layer=",
          "--edit.active-layer=",
          "--edit.geometry-type=",
          "--edit.crs=",
          "--edit.metadata=",
          "--edit.unset-metadata=",
          "--edit.layer-metadata=",
          "--edit.unset-layer-metadata=",
          "--output-format=",
          "--output=",
          "--creation-option=",
          "--layer-creation-option=",
          "--overwrite",
          "--update",
          "--overwrite-layer",
          "--append",
          "--output-layer=",
          "--write.skip-errors",
      ):
        self.assertIn(expected, tokens)
      self.assertNotIn("--edit.nodata=", tokens)

      write_spec(f"gdal pipeline read {poly_shp} ! edit ! edit")
      out, _, _ = utilities_util.run_gdal_cli(
          ["completion", "gdal", "pipeline", pipeline_filename, "-"]
      )
      tokens = out.split(" ")
      self.assertIn("--edit[0].geometry-type=", tokens)
      self.assertIn("--edit[1].geometry-type=", tokens)

      out, _, _ = utilities_util.run_gdal_cli([
          "completion",
          "gdal",
          "pipeline",
          pipeline_filename,
          "--edit[0].geometry-type=",
      ])
      self.assertIn("GEOMETRY", out.split(" "))

  def test_nested_nominal_and_gdalg(self) -> None:
    n43_tif = utilities_util.get_testdata_path("../gdrivers/data/n43.tif")
    color_file = utilities_util.get_testdata_path("data/color_file.txt")
    nested_steps = (
        f"read {n43_tif} ! color-map --color-map {color_file} ! blend"
        f" --operator=hsv-value --overlay [ read {n43_tif} ! hillshade -z 30 ]"
    )

    with gdal.Run(
        "pipeline",
        pipeline=f"{nested_steps} ! write --of MEM unnamed",
    ) as alg:
      ds = alg.Output()
      self.assertEqual(ds.RasterCount, 3)
      self.assertEqual(
          [ds.GetRasterBand(i + 1).Checksum() for i in range(3)],
          [48564, 51840, 50950],
      )

    with utilities_util.vsimem_dir("pipeline_nested_gdalg") as tmp:
      out_filename = f"{tmp}/out.gdalg.json"
      gdal.Run("pipeline", pipeline=f"{nested_steps} ! write {out_filename}")
      with gdal.VSIFile(out_filename, "rb") as f:
        parsed = json.loads(f.read())
      parsed["relative_paths_relative_to_this_file"] = False
      with gdal.VSIFile(out_filename, "wb") as f:
        f.write(json.dumps(parsed).encode("utf-8"))

      del parsed["gdal_version"]
      self.assertEqual(
          parsed,
          {
              "command_line": (
                  f"gdal pipeline read --input {n43_tif} ! color-map"
                  f" --color-map {color_file} ! blend --overlay [ read"
                  f" {n43_tif} ! hillshade -z 30 ] --operator hsv-value"
              ),
              "relative_paths_relative_to_this_file": False,
              "type": "gdal_streamed_alg",
          },
      )

      if utilities_util.has_driver("GDALG"):
        with gdal.Open(out_filename) as ds:
          self.assertEqual(ds.RasterCount, 3)
          self.assertEqual(
              [ds.GetRasterBand(i + 1).Checksum() for i in range(3)],
              [48564, 51840, 50950],
          )

  def test_nested_errors(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    for pipeline_str, err_regex in (
        ("[", "Open bracket must be placed where an input dataset is expected"),
        ("read [", "Open bracket has no matching closing bracket"),
        ("read [ ]", "At least one step must be provided in an inner pipeline"),
        ("read ]", "Closing bracket found without matching open bracket"),
        ("read [ read [ read [ foo ] ] ]", "Too many nested pipelines"),
        (
            "read [ write foo ]",
            "Last step in an inner pipeline must not be a write-like step",
        ),
        (
            (
                f"read [ read {byte_tif} ! write /vsimem/pipeline_err_out.tif !"
                " reproject ] ! info"
            ),
            "'write' is not allowed as an intermediate step",
        ),
        ("read [ read /i/do_not/exist.bin ]", "/i/do_not/exist.bin"),
        (
            (
                f'read [ read {byte_tif} ! reproject --dst-crs "+proj=longlat'
                ' +a=1" ] ! info'
            ),
            "Cannot find coordinate operations",
        ),
    ):
      with self.assertRaisesRegex(RuntimeError, err_regex):
        gdal.Run("pipeline", pipeline=pipeline_str)

  def test_tee_nominal_and_gdalg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    with utilities_util.vsimem_dir("pipeline_tee_ras") as tmp:
      out_filename = f"{tmp}/out.tif"
      with gdal.Run(
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! tee [ write {out_filename} ] ! write --of MEM"
              " mem_dataset"
          ),
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      gdal.Unlink(out_filename)

      out2_filename = f"{tmp}/out2.tif"
      with gdal.Run(
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! tee [ write {out_filename} ] [ write"
              f" {out2_filename} ]"
          ),
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)
      with gdal.Open(out2_filename) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)

    with utilities_util.vsimem_dir("pipeline_tee_vec") as tmp:
      out_shp = f"{tmp}/out.shp"
      with gdal.Run(
          "pipeline",
          pipeline=(
              f"read {poly_shp} ! tee [ write {out_shp} ] ! write --of MEM"
              " mem_dataset"
          ),
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
      with gdal.OpenEx(out_shp, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

      out1_shp = f"{tmp}/out1.shp"
      out2_shp = f"{tmp}/out2.shp"
      with gdal.Run(
          "pipeline",
          pipeline=(
              f'read {poly_shp} ! tee [ filter --where "EAS_ID = 170" ! write'
              f" {out1_shp} ] [ filter --bbox=478315,4763000,481645,4764500 !"
              f" write {out2_shp} ] ! write --of MEM mem_dataset"
          ),
      ) as alg:
        ds = alg.Output()
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
      with gdal.OpenEx(out1_shp, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)
      with gdal.OpenEx(out2_shp, gdal.OF_VECTOR) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 4)

    with utilities_util.vsimem_dir("pipeline_tee_gdalg") as tmp:
      out_shp = f"{tmp}/out.shp"
      gdalg_filename = f"{tmp}/out.gdalg.json"
      with _expect_warning():
        gdal.Run(
            "pipeline",
            pipeline=(
                f"read {poly_shp} ! tee [ write {out_shp} ] ! write"
                f" {gdalg_filename}"
            ),
        )
      self.assertIsNone(gdal.VSIStatL(out_shp))

      if utilities_util.has_driver("GDALG"):
        with self.assertRaisesRegex(
            RuntimeError,
            "Step 'tee' not allowed in stream execution, unless the"
            " GDAL_ALGORITHM_ALLOW_WRITES_IN_STREAM configuration option"
            " is set",
        ):
          gdal.OpenEx(gdalg_filename, gdal.OF_VECTOR)
        self.assertIsNone(gdal.VSIStatL(out_shp))

        with gdal.config_option("GDAL_ALGORITHM_ALLOW_WRITES_IN_STREAM", "YES"):
          with gdal.OpenEx(gdalg_filename, gdal.OF_VECTOR) as ds:
            self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)
        self.assertIsNotNone(gdal.VSIStatL(out_shp))
        with gdal.OpenEx(out_shp, gdal.OF_VECTOR) as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 10)

  def test_tee_errors_and_outputs(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

    with utilities_util.vsimem_dir("pipeline_tee_err") as tmp:
      for pipeline_str, err_regex in (
          (
              f"read {byte_tif} ! tee [ ]",
              "At least one step must be provided in an inner pipeline",
          ),
          (f"read {byte_tif} ! tee [ invalid ]", "unknown step name: invalid"),
          (
              f"read {byte_tif} ! tee illegal",
              "'illegal' is not a valid nested pipeline",
          ),
          (
              (
                  f"read {byte_tif} ! tee [ write {tmp}/out.shp ] [ write"
                  f" {tmp}/out.shp ]"
              ),
              "Another identical nested pipeline exists",
          ),
          (
              f"read {byte_tif} ! tee [ read {tmp}/out.shp ]",
              "No read-like step like 'read' is allowed",
          ),
          (
              (
                  f"read {poly_shp} ! tee [ filter --bbox=0,1,1,0 ! write"
                  f" {tmp}/out.shp ]"
              ),
              (
                  "Value of 'bbox' should be xmin,ymin,xmax,ymax with xmin <="
                  " xmax and ymin <= ymax"
              ),
          ),
      ):
        with self.assertRaisesRegex(RuntimeError, err_regex):
          gdal.Run("pipeline", pipeline=pipeline_str)

      out_tif = f"{tmp}/out.tif"
      gdal.Run(
          "pipeline", pipeline=f"read {byte_tif} ! tee [ write {out_tif} ]"
      )
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        gdal.Run(
            "pipeline", pipeline=f"read {byte_tif} ! tee [ write {out_tif} ]"
        )

      tmp_tif = f"{tmp}/tmp.tif"
      with gdal.Run(
          "pipeline",
          pipeline=f"read {byte_tif} ! tee [ write {tmp_tif} ] [ info ]",
      ) as alg:
        with self.assertRaisesRegex(
            RuntimeError,
            r"Cannot use 'output' method on this algorithm as it supports"
            r" multiple output arguments\. Use 'Outputs' \(plural\) instead",
        ):
          alg.Output()

      with gdal.Run(
          "pipeline",
          pipeline=(
              f"read {byte_tif} ! tee [ write {tmp_tif} --overwrite ] [ info ]"
          ),
      ) as alg:
        outputs = alg.Outputs()
        self.assertEqual(outputs["output"].GetDescription(), byte_tif)
        self.assertEqual(outputs["output-string"]["description"], byte_tif)

    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out, _, code = utilities_util.run_gdal_cli([
          "pipeline",
          "read",
          byte_tif,
          "!",
          "tee",
          "[",
          "write",
          f"{tmpdir}/tmp.tif",
          "]",
          "[",
          "info",
          "]",
          "!",
          "select",
          "--band",
          "1",
          "!",
          "write",
          f"{tmpdir}/tmp2.tif",
      ])
      self.assertEqual(code, 0)
      self.assertTrue(out.startswith("Driver: GTiff/GeoTIFF"), out)

  def test_tee_output_to_stdout(self) -> None:
    if not utilities_util.has_driver("XYZ"):
      self.skipTest("XYZ driver not available")

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    out, _, code = utilities_util.run_gdal_cli([
        "pipeline",
        "read",
        byte_tif,
        "!",
        "tee",
        "[",
        "write",
        "--of",
        "XYZ",
        "/vsistdout/",
        "]",
    ])
    self.assertEqual(code, 0)

    with utilities_util.vsimem_dir("pipeline_tee_stdout") as tmp:
      xyz_path = f"{tmp}/test.xyz"
      gdal.FileFromMemBuffer(xyz_path, out)
      with gdal.Open(xyz_path) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 4672)


if __name__ == "__main__":
  absltest.main()
