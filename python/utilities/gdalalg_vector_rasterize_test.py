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

"""Tests for `gdal vector rasterize`.

Ported from upstream autotest/utilities/test_gdalalg_vector_rasterize.py.

Upstream copyright:
  Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator
import contextlib

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_CUTLINE_CSV = (
    "Counter,HEIGHT,WKT\n"
    '1,100,"POLYGON((6.25 1.25 100,7.25 1.25 200,7.25 2.25 250,'
    '6.25 2.25 254,6.25 1.25 100))"\n'
    '2,200,"POLYGON((4.25 4.25 220,6.25 4.25 200,6.25 6.25 202,'
    '4.25 6.25 200,4.25 4.25 220))"\n'
    '3,300,"POLYGON((1.001 1.001 200,3.999 3.999 220,3.2 1.6 210,'
    '1.001 1.001 200))"\n'
)


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_rasterize_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["vector"]["rasterize"]


@contextlib.contextmanager
def _temp_cutline(input_csv: str) -> Iterator[None]:
  gdal.FileFromMemBuffer(input_csv, _CUTLINE_CSV)
  try:
    yield
  finally:
    gdal.Unlink(input_csv)


class GdalalgVectorRasterizeTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      (
          True,
          [
              "--all-touched",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "already exists",
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          121,
      ),
      (
          False,
          [
              "--all-touched",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "Must specify output resolution (--resolution) or size (--size)",
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "--where",
              "Counter='2'",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          46,
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "--sql",
              "SELECT * FROM cutline WHERE Counter='2'",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
          ],
          46,
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "--sql",
              "SELECT * FROM cutline WHERE Counter='2'",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "Argument 'sql' is mutually exclusive with 'input-layer'.",
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
              "--3d",
          ],
          "Argument '-3d' not allowed with '-burn <value>'",
      ),
      (
          True,
          ["--update", "--all-touched", "-b", "3,2,1", "-l", "cutline", "--3d"],
          101,
      ),
      (
          True,
          [
              "--update",
              "--invert",
              "--all-touched",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          1690,
      ),
      (
          True,
          [
              "--update",
              "--attribute-name",
              "HEIGHT",
              "--all-touched",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "Argument '-a <attribute_name>' not allowed with '-burn <value>'",
      ),
      (
          True,
          [
              "--update",
              "--attribute-name",
              "__HEIGHT",
              "--all-touched",
              "-b",
              "3,2,1",
              "-l",
              "cutline",
          ],
          "Failed to find field __HEIGHT on layer cutline",
      ),
      (
          True,
          [
              "--update",
              "--attribute-name",
              "HEIGHT",
              "--all-touched",
              "-b",
              "3,2,1",
              "-l",
              "cutline",
          ],
          168,
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "--dialect",
              "SQLITE",
              "--sql",
              "SELECT * FROM cutline WHERE Counter='2'",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
          ],
          46,
      ),
      (
          True,
          [
              "--update",
              "--all-touched",
              "--dialect",
              "XXXXXX",
              "--sql",
              "SELECT * FROM cutline WHERE Counter='2'",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
          ],
          46,
      ),
      (
          False,
          [
              "--all-touched",
              "--init",
              "100,200,300",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "Must specify output resolution (--resolution) or size (--size)",
      ),
      (
          False,
          [
              "--all-touched",
              "--size",
              "10,10",
              "--init",
              "100,200,300",
              "-b",
              "3,2,1",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "-b option cannot be used when creating a GDAL dataset.",
      ),
      (
          False,
          [
              "--all-touched",
              "--size",
              "10,10",
              "--init",
              "100,200,300",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          1418,
      ),
      (
          False,
          [
              "--all-touched",
              "--resolution",
              "0.6249,0.5249",
              "--size",
              "10,10",
              "--init",
              "100,200,300",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "Argument 'size' is mutually exclusive with 'resolution'.",
      ),
      (
          False,
          [
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          500,
      ),
      (
          False,
          [
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--extent",
              "0.64,1.0,7.64,6.50",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          524,
      ),
      (
          False,
          [
              "--tap",
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          497,
      ),
      (
          False,
          [
              "--optimization",
              "XXXXX",
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          (
              "Invalid value 'XXXXX' for string argument 'optimization'."
              " Should be one among 'AUTO', 'RASTER', 'VECTOR'."
          ),
      ),
      (
          False,
          [
              "--optimization",
              "AUTO",
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          500,
      ),
      (
          False,
          [
              "--crs",
              "EPSG:XXXXX",
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          "Invalid value for 'crs' argument",
      ),
      (
          False,
          [
              "--crs",
              "EPSG:4326",
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          500,
      ),
      (
          False,
          [
              "--nodata",
              "-1",
              "--all-touched",
              "--resolution",
              "0.7,0.5",
              "--burn",
              "200,220,240",
              "-l",
              "cutline",
          ],
          431,
      ),
  )
  def test_rasterize(
      self,
      create_empty_dataset: bool,
      options: list[str],
      expected: str | int,
  ) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    with utilities_util.vsimem_dir("vector_rasterize") as tmp:
      input_csv = f"{tmp}/cutline.csv"
      output_tif = f"{tmp}/rasterize_alg_1.tif"
      with _temp_cutline(input_csv):
        self.assertIsNone(gdal.VSIStatL(output_tif))
        if create_empty_dataset:
          with gdal.GetDriverByName("GTiff").Create(
              output_tif, 12, 12, 3, gdal.GDT_Byte
          ) as target_ds:
            target_ds.SetGeoTransform((0, 1, 0, 12, 0, -1))

        if isinstance(expected, str):
          with self.assertRaises(RuntimeError) as ctx:
            with gdal.quiet_errors():
              _get_rasterize_alg().ParseRunAndFinalize(
                  [*options, input_csv, output_tif]
              )
          self.assertIn(expected, str(ctx.exception))
          return

        with gdal.quiet_errors():
          self.assertTrue(
              _get_rasterize_alg().ParseRunAndFinalize(
                  [*options, input_csv, output_tif]
              )
          )

        with gdal.Open(output_tif) as target_ds:
          checksum = target_ds.GetRasterBand(2).Checksum()
          self.assertEqual(checksum, expected)
          if "--crs" in options:
            self.assertTrue(
                target_ds.GetProjection().startswith('GEOGCS["WGS 84"')
            )
          else:
            self.assertEqual(target_ds.GetProjection(), "")

          if "--nodata" in options:
            self.assertEqual(target_ds.GetRasterBand(2).GetNoDataValue(), -1)
          else:
            self.assertIn(
                target_ds.GetRasterBand(2).GetNoDataValue(), (None, 0)
            )

  def test_add_option(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    options = [
        "--all-touched",
        "-b",
        "3,2,1",
        "--burn",
        "200,220,240",
    ]
    with utilities_util.vsimem_dir("vector_rasterize_add") as tmp:
      input_csv = f"{tmp}/cutline.csv"
      output_tif = f"{tmp}/rasterize_alg_1.tif"
      with gdal.GetDriverByName("GTiff").Create(
          output_tif, 12, 12, 3, gdal.GDT_Byte
      ) as target_ds:
        target_ds.SetGeoTransform((0, 1, 0, 12, 0, -1))

      with _temp_cutline(input_csv):
        last_pct = [0.0]

        def my_progress(pct: float, msg: str, user_data: object) -> bool:
          del msg, user_data
          last_pct[0] = pct
          return True

        self.assertTrue(
            _get_rasterize_alg().ParseRunAndFinalize(
                [*options, "--update", input_csv, output_tif],
                my_progress,
            )
        )
        self.assertEqual(last_pct[0], 1.0)
        with gdal.Open(output_tif) as target_ds:
          self.assertEqual(target_ds.GetRasterBand(2).Checksum(), 121)

        self.assertTrue(
            _get_rasterize_alg().ParseRunAndFinalize(
                [*options, "--add", input_csv, output_tif]
            )
        )
        with gdal.Open(output_tif) as target_ds:
          self.assertEqual(target_ds.GetRasterBand(2).Checksum(), 166)

  def test_dialect_warning(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    expected_warning = (
        "Dialect 'XXXXXX' is unsupported. Only supported dialects are"
        " 'OGRSQL', 'SQLITE'. Defaulting to OGRSQL"
    )
    with utilities_util.vsimem_dir("vector_rasterize_dialect") as tmp:
      input_csv = f"{tmp}/cutline.csv"
      output_tif = f"{tmp}/rasterize_alg_1.tif"
      with gdal.GetDriverByName("GTiff").Create(
          output_tif, 12, 12, 3, gdal.GDT_Byte
      ) as target_ds:
        target_ds.SetGeoTransform((0, 1, 0, 12, 0, -1))

      options = [
          "--all-touched",
          "--dialect",
          "XXXXXX",
          "--sql",
          "SELECT * FROM cutline WHERE Counter='2'",
          "-b",
          "3,2,1",
          "--burn",
          "200,220,240",
          "--update",
      ]
      with _temp_cutline(input_csv):
        messages: list[tuple[int, str]] = []

        def handler(err_level: int, unused_err_no: int, err_msg: str) -> None:
          messages.append((err_level, err_msg))

        gdal.PushErrorHandler(handler)
        try:
          self.assertTrue(
              _get_rasterize_alg().ParseRunAndFinalize(
                  [*options, input_csv, output_tif]
              )
          )
        finally:
          gdal.PopErrorHandler()
        self.assertTrue(
            any(
                lvl == gdal.CE_Warning and expected_warning in msg
                for lvl, msg in messages
            ),
            f"Expected warning {expected_warning!r} in {messages!r}",
        )

  def test_overwrite(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    with utilities_util.vsimem_dir("vector_rasterize_overwrite") as tmp:
      input_csv = f"{tmp}/cutline.csv"
      output_tif = f"{tmp}/rasterize_alg_1.tif"
      with gdal.GetDriverByName("GTiff").Create(
          output_tif, 12, 12, 3, gdal.GDT_Byte
      ) as target_ds:
        target_ds.SetGeoTransform((0, 1, 0, 12, 0, -1))

      with _temp_cutline(input_csv):
        with self.assertRaisesRegex(RuntimeError, "already exists"):
          _get_rasterize_alg().ParseRunAndFinalize(
              ["-b", "1,2,3", "--burn=200,220,240", input_csv, output_tif]
          )

        self.assertTrue(
            _get_rasterize_alg().ParseRunAndFinalize([
                "--co",
                "TILED=YES",
                "--burn=200,220,240",
                input_csv,
                output_tif,
                "--size=10,11",
                "--overwrite",
            ])
        )
        with gdal.Open(output_tif) as ds:
          self.assertEqual(ds.RasterXSize, 10)
          self.assertEqual(ds.RasterYSize, 11)
          self.assertEqual(ds.GetRasterBand(1).GetBlockSize(), [256, 256])

  def test_missing_size_and_res(self) -> None:
    rasterize = _get_rasterize_alg()
    rasterize["input"] = utilities_util.get_testdata_path(
        "../ogr/data/poly.shp"
    )
    rasterize["burn"] = 1
    rasterize["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "--resolution.*or.*--size"):
      rasterize.Run()


if __name__ == "__main__":
  absltest.main()
