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

"""Tests for `gdal raster create`.

Ported from upstream autotest/utilities/test_gdalalg_raster_create.py.

Upstream copyright:
  Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import math

from absl.testing import absltest
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["create"]


def _get_ref_ds() -> gdal.Dataset:
  alg = _get_alg()
  alg["output"] = ""
  alg["output-format"] = "MEM"
  alg["size"] = [2, 4]
  alg["band-count"] = 2
  alg["nodata"] = 255
  alg["burn"] = [253, 254]
  alg["datatype"] = "UInt16"
  alg["crs"] = "WGS84"
  alg["bbox"] = [2, 49, 3, 50]
  alg["metadata"] = {"key": "value"}
  if not alg.Run():
    raise RuntimeError("Failed to create reference dataset")
  ds = utilities_util.get_arg_dataset(alg, "output")
  ds.BuildOverviews("NEAR", [2])
  ds.GetRasterBand(1).SetMetadataItem("foo", "bar")
  return ds


class GdalalgRasterCreateTest(gdaltest.TestCase):

  def test_missing_size(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError,
        "create: Argument 'size' should be specified, or 'like' dataset should"
        " be specified",
    ):
      alg.Run()

  def test_cannot_create_file(self) -> None:
    alg = _get_alg()
    alg["output"] = "/i_do/not/exist/out.tif"
    alg["size"] = [1, 1]
    with self.assertRaisesRegex(
        RuntimeError, "Attempt to create new tiff file"
    ):
      alg.Run()

  def test_cannot_guess_format(self) -> None:
    alg = _get_alg()
    alg["output"] = "/i_do/not/exist/out.unk"
    alg["size"] = [1, 1]
    with self.assertRaisesRegex(
        RuntimeError, "Cannot guess driver for /i_do/not/exist/out.unk"
    ):
      alg.Run()

  def test_minimal(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["size"] = [2, 3]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterCount, 1)
    self.assertEqual(ds.RasterXSize, 2)
    self.assertEqual(ds.RasterYSize, 3)
    self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Byte)
    self.assertIsNone(ds.GetRasterBand(1).GetNoDataValue())

  def test_burn_invalid_count(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["size"] = [1, 1]
    alg["burn"] = [1, 2]
    with self.assertRaisesRegex(
        RuntimeError,
        "One value should be provided for argument 'burn', given there is one"
        " band",
    ):
      alg.Run()

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["size"] = [1, 1]
    alg["band-count"] = 3
    alg["burn"] = [1, 2]
    with self.assertRaisesRegex(
        RuntimeError,
        "3 values should be provided for argument 'burn', given there are 3"
        " bands",
    ):
      alg.Run()

  def test_overwrite_and_append(self) -> None:
    with utilities_util.vsimem_dir("raster_create_overwrite") as tmp:
      out_filename = f"{tmp}/test.tif"

      alg = _get_alg()
      alg["output"] = out_filename
      alg["size"] = [1, 1]
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 1)
        self.assertEqual(ds.RasterYSize, 1)

      alg = _get_alg()
      alg["output"] = out_filename
      alg["size"] = [1, 1]
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        alg.Run()

      alg = _get_alg()
      alg["output"] = out_filename
      alg["size"] = [2, 3]
      alg["append"] = True
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 1)
        self.assertEqual(ds.RasterYSize, 1)
        self.assertLen(ds.GetSubDatasets(), 2)

      alg = _get_alg()
      alg["output"] = out_filename
      alg["size"] = [2, 3]
      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      with gdal.Open(out_filename) as ds:
        self.assertEqual(ds.RasterCount, 1)
        self.assertEqual(ds.RasterXSize, 2)
        self.assertEqual(ds.RasterYSize, 3)
        self.assertEmpty(ds.GetSubDatasets())

  def test_failed_append(self) -> None:
    if not utilities_util.has_driver("HFA"):
      self.skipTest("HFA driver not available")

    with utilities_util.vsimem_dir("raster_create_hfa") as tmp:
      out_filename = f"{tmp}/test.img"

      alg = _get_alg()
      alg["output"] = out_filename
      alg["output-format"] = "HFA"
      alg["size"] = [1, 1]
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())

      alg = _get_alg()
      alg["output"] = out_filename
      alg["output-format"] = "HFA"
      alg["size"] = [1, 1]
      alg["append"] = True
      with self.assertRaisesRegex(
          RuntimeError, "-append option not supported for driver HFA"
      ):
        alg.Run()

  def test_full(self) -> None:
    ds = _get_ref_ds()
    self.assertEqual(ds.RasterCount, 2)
    self.assertEqual(ds.RasterXSize, 2)
    self.assertEqual(ds.RasterYSize, 4)
    self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 255)
    self.assertEqual(
        ds.GetRasterBand(1).ReadRaster(0, 0, 1, 1, buf_type=gdal.GDT_Byte),
        b"\xFD",
    )
    self.assertEqual(
        ds.GetRasterBand(2).ReadRaster(0, 0, 1, 1, buf_type=gdal.GDT_Byte),
        b"\xFE",
    )
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "4326")
    self.assertEqual(ds.GetGeoTransform(), (2.0, 0.5, 0.0, 50.0, 0.0, -0.25))
    self.assertEqual(ds.GetMetadataItem("key"), "value")

  def test_copy(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterCount, 2)
    self.assertEqual(ds.RasterXSize, 2)
    self.assertEqual(ds.RasterYSize, 4)
    self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_UInt16)
    self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 255)
    self.assertEqual(
        ds.GetRasterBand(1).ReadRaster(0, 0, 1, 1, buf_type=gdal.GDT_Byte),
        b"\x00",
    )
    self.assertEqual(
        ds.GetRasterBand(2).ReadRaster(0, 0, 1, 1, buf_type=gdal.GDT_Byte),
        b"\x00",
    )
    self.assertEqual(ds.GetSpatialRef().GetAuthorityCode(None), "4326")
    self.assertEqual(ds.GetGeoTransform(), (2.0, 0.5, 0.0, 50.0, 0.0, -0.25))
    self.assertIsNone(ds.GetMetadataItem("key"))
    self.assertIsNone(ds.GetRasterBand(1).GetMetadataItem("foo"))
    self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 0)

  def test_copy_metadata(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["copy-metadata"] = True
    alg["size"] = [1, 1]
    with self.assertRaisesRegex(
        RuntimeError,
        "Argument 'copy-metadata' can only be set when an input dataset is set",
    ):
      alg.Run()

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["copy-metadata"] = True
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetMetadataItem("key"), "value")
    self.assertEqual(ds.GetRasterBand(1).GetMetadataItem("foo"), "bar")

  def test_copy_overviews(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["copy-overviews"] = True
    alg["size"] = [1, 1]
    with self.assertRaisesRegex(
        RuntimeError,
        "Argument 'copy-overviews' can only be set when an input dataset is"
        " set",
    ):
      alg.Run()

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["copy-overviews"] = True
    alg["size"] = [1, 1]
    with self.assertRaisesRegex(
        RuntimeError,
        "Argument 'copy-overviews' can only be set when the input and output"
        " datasets have the same dimension",
    ):
      alg.Run()

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["copy-overviews"] = True
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.GetRasterBand(1).GetOverviewCount(), 1)

  def test_copy_unset_crs_and_nodata(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["crs"] = "none"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertIsNone(ds.GetSpatialRef())

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["nodata"] = "none"
    self.assertTrue(alg.Run())
    ds2 = utilities_util.get_arg_dataset(alg, "output")
    band2 = ds2.GetRasterBand(1)
    self.assertIsNone(band2.GetNoDataValue())

  def test_nodata_nan(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["size"] = [1, 1]
    alg["datatype"] = "Float64"
    alg["nodata"] = "nan"
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    band = ds.GetRasterBand(1)
    nodata = band.GetNoDataValue()
    self.assertIsNotNone(nodata)
    assert nodata is not None
    self.assertTrue(math.isnan(nodata))

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["size"] = [1, 1]
    alg["nodata"] = "nan"
    with self.assertRaisesRegex(
        RuntimeError,
        "Setting nodata value failed as it cannot be represented on its data"
        " type",
    ):
      alg.Run()

  def test_copy_overrides(self) -> None:
    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["size"] = [1, 1]
    self.assertTrue(alg.Run())
    ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds.RasterXSize, 1)
    self.assertEqual(ds.RasterYSize, 1)
    self.assertEqual(ds.RasterCount, 2)

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["band-count"] = 1
    self.assertTrue(alg.Run())
    ds2 = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds2.RasterXSize, 2)
    self.assertEqual(ds2.RasterYSize, 4)
    self.assertEqual(ds2.RasterCount, 1)

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["datatype"] = "Byte"
    self.assertTrue(alg.Run())
    ds3 = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(ds3.GetRasterBand(1).DataType, gdal.GDT_Byte)

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["crs"] = "EPSG:32631"
    self.assertTrue(alg.Run())
    ds4 = utilities_util.get_arg_dataset(alg, "output")
    srs = ds4.GetSpatialRef()
    self.assertIsNotNone(srs)
    assert srs is not None
    self.assertEqual(srs.GetAuthorityCode(None), "32631")

    alg = _get_alg()
    alg["output"] = ""
    alg["output-format"] = "MEM"
    alg["input"] = _get_ref_ds()
    alg["bbox"] = [1, 47, 2, 48]
    self.assertTrue(alg.Run())
    ds5 = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(
        ds5.GetGeoTransform(),
        (1.0, 0.5, 0.0, 48.0, 0.0, -0.25),
    )

  def test_creation_option(self) -> None:
    with utilities_util.vsimem_dir("raster_create_co") as tmp:
      out_filename = f"{tmp}/out.tif"
      alg = _get_alg()
      alg["output"] = out_filename
      alg["size"] = [1, 1]
      alg["creation-option"] = {"COMPRESS": "LZW"}
      self.assertTrue(alg.Run())
      self.assertTrue(alg.Finalize())
      with gdal.Open(out_filename) as ds:
        self.assertEqual(
            ds.GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW"
        )

  def test_overwrite_mem_file_with_real_file_same_name(self) -> None:
    with utilities_util.vsimem_dir("raster_create_same_name") as tmp:
      out_filename = f"{tmp}/out.tif"
      gdal.FileFromMemBuffer(out_filename, "")

      alg = _get_alg()
      alg["output"] = out_filename
      alg["output-format"] = "MEM"
      alg["size"] = [1, 1]
      alg["overwrite"] = True
      self.assertTrue(alg.Run())
      self.assertIsNotNone(gdal.VSIStatL(out_filename))

  def test_driver_not_available(self) -> None:
    alg = _get_alg()
    alg["output-format"] = "MEM"
    alg["size"] = [1, 1]

    drv = gdal.GetDriverByName("MEM")
    drv.Deregister()
    try:
      with self.assertRaisesRegex(RuntimeError, "Driver 'MEM' does not exist"):
        alg.Run()
    finally:
      drv.Register()

  def test_set_crs_failed(self) -> None:
    if not utilities_util.has_driver("NITF"):
      self.skipTest("NITF driver not available")

    with utilities_util.vsimem_dir("raster_create_nitf") as tmp:
      alg = _get_alg()
      alg["output"] = f"{tmp}/out.ntf"
      alg["crs"] = "+proj=ortho +datum=WGS84"
      alg["size"] = [1, 1]
      with self.assertRaisesRegex(
          RuntimeError,
          "NITF only supports WGS84 geographic and UTM projections",
      ):
        alg.Run()

  def test_set_bbox_failed_null_dimension(self) -> None:
    alg = _get_alg()
    alg["output-format"] = "MEM"
    alg["bbox"] = [0, 0, 0, 0]
    alg["size"] = [0, 0]
    with self.assertRaisesRegex(
        RuntimeError,
        "Cannot set extent because one of dataset height or width is null",
    ):
      alg.Run()

  def test_set_bbox_failed(self) -> None:
    if not utilities_util.has_driver("GPKG"):
      self.skipTest("GPKG driver not available")

    with utilities_util.vsimem_dir("raster_create_gpkg") as tmp:
      alg = _get_alg()
      alg["output"] = f"{tmp}/out.gpkg"
      alg["bbox"] = [0, 0, 0, 0]
      alg["size"] = [1, 1]
      with self.assertRaisesRegex(RuntimeError, "Setting extent failed"):
        alg.Run()

  def test_empty_bbox(self) -> None:
    alg = _get_alg()
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError,
        "0 value has been specified for argument 'bbox', whereas exactly 4 were"
        " expected",
    ):
      alg["bbox"] = []
    alg["size"] = [1, 1]
    with self.assertRaisesRegex(
        RuntimeError,
        "0 value has been specified for argument 'bbox', whereas exactly 4 were"
        " expected",
    ):
      alg.Run()


if __name__ == "__main__":
  absltest.main()
