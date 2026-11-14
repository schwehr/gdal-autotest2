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

"""Tests for `gdal raster pansharpen`.

Ported from upstream autotest/utilities/test_gdalalg_raster_pansharpen.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator, Mapping
import contextlib

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util

_EXPECTED_CHECKSUMS = (
    [4735, 10000, 9742],
    [4731, 9991, 9734],
    [4726, 10004, 9727],
)


def setUpModule() -> None:
  utilities_util.setup_test_env()


@contextlib.contextmanager
def _small_world_pan_tif(dir_name: str) -> Iterator[str]:
  small_world = utilities_util.get_testdata_path(
      "../gdrivers/data/small_world.tif"
  )
  with utilities_util.vsimem_dir(dir_name) as tmp:
    pan_path = f"{tmp}/small_world_pan.tif"
    with gdal.Open(small_world) as src_ds:
      src_data = src_ds.GetRasterBand(1).ReadRaster()
      gt = list(src_ds.GetGeoTransform())
      wkt = src_ds.GetProjectionRef()

    with gdal.GetDriverByName("GTiff").Create(pan_path, 800, 400) as pan_ds:
      gt[1] *= 0.5
      gt[5] *= 0.5
      pan_ds.SetGeoTransform(gt)
      pan_ds.SetProjection(wkt)
      pan_ds.GetRasterBand(1).WriteRaster(0, 0, 800, 400, src_data, 400, 200)

    yield pan_path


class GdalalgRasterPansharpenTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      ({"weights": [1.0 / 3, 1.0 / 3, 1.0 / 3]},),
      ({"bit-depth": 8},),
  )
  def test_basic(self, options: Mapping[str, object]) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with _small_world_pan_tif("pansharpen_basic") as pan_tif:
      with gdal.Run(
          "raster",
          "pansharpen",
          input=pan_tif,
          spectral=small_world,
          output="",
          format="VRT",
          **options,
      ) as alg:
        ds = alg.Output()
        cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)]
        self.assertIn(cs, _EXPECTED_CHECKSUMS)

  def test_pipeline(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with _small_world_pan_tif("pansharpen_pipeline") as pan_tif:
      pipeline = (
          f"read {pan_tif} ! pansharpen {small_world} !"
          " write streamed_data --of=stream"
      )
      with gdal.Run("raster", "pipeline", pipeline=pipeline) as alg:
        ds = alg.Output()
        cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)]
        self.assertIn(cs, _EXPECTED_CHECKSUMS)

  def test_ds_per_reference(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with _small_world_pan_tif("pansharpen_ref") as pan_tif:
      with gdal.Open(pan_tif) as pan_ds, gdal.Open(small_world) as spec_ds:
        with gdal.Run(
            "raster",
            "pansharpen",
            input=pan_ds,
            spectral=spec_ds,
            output="",
            format="VRT",
        ) as alg:
          ds = alg.Output()
          cs = [
              ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)
          ]
          self.assertIn(cs, _EXPECTED_CHECKSUMS)

  def test_spectral_with_band_suffix(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with _small_world_pan_tif("pansharpen_bands") as pan_tif:
      with gdal.Run(
          "raster",
          "pansharpen",
          input=pan_tif,
          spectral=[
              f"{small_world},band=1",
              f"{small_world},band=2",
              f"{small_world},band=3",
          ],
          output="",
          format="VRT",
      ) as alg:
        ds = alg.Output()
        cs = [ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)]
        self.assertIn(cs, _EXPECTED_CHECKSUMS)

  @parameterized.parameters(
      ({"weights": [1.0 / 3, 1.0 / 3, 0]},),
      ({"nodata": 1},),
  )
  def test_full_options(self, options: Mapping[str, object]) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    with _small_world_pan_tif("pansharpen_opts") as pan_tif:
      with gdal.Open(small_world) as spec_ds:
        with gdal.Run(
            "raster",
            "pansharpen",
            input=pan_tif,
            spectral=spec_ds,
            output="",
            format="VRT",
            **options,
        ) as alg:
          ds = alg.Output()
          cs = [
              ds.GetRasterBand(i + 1).Checksum() for i in range(ds.RasterCount)
          ]
          self.assertNotIn(cs, _EXPECTED_CHECKSUMS)

  def test_errors(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")

    with self.assertRaisesRegex(
        RuntimeError, "Input panchromatic dataset must have a single band"
    ):
      gdal.Run(
          "raster",
          "pansharpen",
          input=small_world,
          spectral=small_world,
          output="",
          format="VRT",
      )

    with self.assertRaisesRegex(RuntimeError, "/i/do_not/exist.xxx"):
      gdal.Run(
          "raster",
          "pansharpen",
          input=byte_tif,
          spectral="/i/do_not/exist.xxx",
          output="",
          format="VRT",
      )

    with self.assertRaisesRegex(RuntimeError, "Illegal band"):
      gdal.Run(
          "raster",
          "pansharpen",
          input=byte_tif,
          spectral=[f"{small_world},band=4"],
          output="",
          format="VRT",
      )

    with gdal.GetDriverByName("MEM").Create("", 1, 1) as mem_ds:
      with self.assertRaisesRegex(
          RuntimeError, "Panchromatic band has no associated geotransform"
      ):
        gdal.Run(
            "raster",
            "pansharpen",
            input=mem_ds,
            spectral=small_world,
            output="",
            format="VRT",
        )


if __name__ == "__main__":
  absltest.main()
