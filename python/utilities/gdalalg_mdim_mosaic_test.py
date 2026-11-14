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

"""Tests for `gdal mdim mosaic`.

Ported from upstream autotest/utilities/test_gdalalg_mdim_mosaic.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import array
from collections.abc import Sequence
import os

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _create_vertical_z_array(
    rg: gdal.Group, z_dim: gdal.Dimension, z_vals: Sequence[float]
) -> None:
  z_ar = rg.CreateMDArray(
      "z", [z_dim], gdal.ExtendedDataType.Create(gdal.GDT_Float64)
  )
  z_ar.CreateAttribute(
      "axis", [1], gdal.ExtendedDataType.CreateString()
  ).WriteString("Z")
  z_ar.CreateAttribute(
      "positive", [1], gdal.ExtendedDataType.CreateString()
  ).WriteString("up")
  z_ar.Write(list(z_vals))


def _create_1d_z_nc(
    path: str,
    z_vals: Sequence[float],
    data_vals: Sequence[int],
    *,
    with_z_attrs: bool = True,
    dim_name: str = "z",
    dtype: int = gdal.GDT_Byte,
    nodata: float | None = None,
) -> None:
  with gdal.GetDriverByName("netCDF").CreateMultiDimensional(path) as ds:
    rg = ds.GetRootGroup()
    z = rg.CreateDimension(dim_name, "VERTICAL", "UP", len(z_vals))
    if with_z_attrs and dim_name == "z":
      _create_vertical_z_array(rg, z, z_vals)
    else:
      z_ar = rg.CreateMDArray(
          dim_name, [z], gdal.ExtendedDataType.Create(gdal.GDT_Float64)
      )
      z_ar.Write(list(z_vals))
    ar = rg.CreateMDArray("test", [z], gdal.ExtendedDataType.Create(dtype))
    if nodata is not None:
      ar.SetNoDataValue(nodata)
    typecode = "I" if dtype == gdal.GDT_Int32 else "B"
    ar.Write(array.array(typecode, data_vals))


def _create_2d_z_dim1_nc(
    path: str,
    z_vals: Sequence[float],
    dim1_vals: Sequence[float],
    arrays: Sequence[tuple[str, Sequence[int] | None]],
    creation_options: Sequence[str] | None = None,
) -> None:
  with gdal.GetDriverByName("netCDF").CreateMultiDimensional(path) as ds:
    rg = ds.GetRootGroup()
    z = rg.CreateDimension("z", "VERTICAL", "UP", len(z_vals))
    _create_vertical_z_array(rg, z, z_vals)
    dim1 = rg.CreateDimension("dim1", None, None, len(dim1_vals))
    dim1_ar = rg.CreateMDArray(
        "dim1", [dim1], gdal.ExtendedDataType.Create(gdal.GDT_Float64)
    )
    dim1_ar.Write(array.array("d", dim1_vals))
    for arr_name, data_vals in arrays:
      if creation_options is not None:
        ar = rg.CreateMDArray(
            arr_name,
            [z, dim1],
            gdal.ExtendedDataType.Create(gdal.GDT_Byte),
            list(creation_options),
        )
      else:
        ar = rg.CreateMDArray(
            arr_name, [z, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )
      if data_vals is not None:
        ar.Write(array.array("B", data_vals))


class GdalalgMdimMosaicTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    if not utilities_util.has_driver("netCDF"):
      self.skipTest("netCDF driver not available")

  def test_labelled_axis_single_value_1d_and_glob(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      _create_1d_z_nc(os.path.join(tmpdir, "test1.nc"), [10], [3])
      _create_1d_z_nc(os.path.join(tmpdir, "test2.nc"), [20], [4])
      _create_1d_z_nc(os.path.join(tmpdir, "test3.nc"), [0], [2])

      with gdal.Run(
          "mdim",
          "mosaic",
          input=os.path.join(tmpdir, "test*.nc"),
          output=os.path.join(tmpdir, "out.vrt"),
          array="test",
          output_format="VRT",
      ) as alg:
        ds = alg.Output()
        ar = ds.GetRootGroup().OpenMDArray("test")
        dims = ar.GetDimensions()
        self.assertEqual(dims[0].GetName(), "z")
        self.assertEqual(dims[0].GetType(), "VERTICAL")
        self.assertEqual(dims[0].GetDirection(), "UP")
        self.assertEqual(dims[0].GetSize(), 3)
        self.assertEqual(
            array.array("d", dims[0].GetIndexingVariable().Read()),
            array.array("d", [0, 10, 20]),
        )
        self.assertEqual(
            array.array("B", ar.Read()), array.array("B", [2, 3, 4])
        )

  def test_labelled_axis_multiple_values_1d_and_file_list(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_1d_z_nc(p1, [10, 30], [3, 4])
      _create_1d_z_nc(p2, [100, 200, 250], [5, 6, 7])

      list_path = os.path.join(tmpdir, "list.txt")
      with open(list_path, "wb") as f:
        f.write(f"{p1}\n{p2}\n".encode("utf-8"))

      with gdal.Run(
          "mdim",
          "mosaic",
          input=f"@{list_path}",
          output=os.path.join(tmpdir, "out.vrt"),
          array="test",
          output_format="VRT",
      ) as alg:
        ds = alg.Output()
        ar = ds.GetRootGroup().OpenMDArray("test")
        dims = ar.GetDimensions()
        self.assertEqual(dims[0].GetName(), "z")
        self.assertEqual(dims[0].GetType(), "VERTICAL")
        self.assertEqual(dims[0].GetDirection(), "UP")
        self.assertEqual(dims[0].GetSize(), 5)
        self.assertEqual(
            array.array("d", dims[0].GetIndexingVariable().Read()),
            array.array("d", [10, 30, 100, 200, 250]),
        )
        self.assertEqual(
            array.array("B", ar.Read()), array.array("B", [3, 4, 5, 6, 7])
        )

  def test_regularly_spaced_axis_1d(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_1d_z_nc(p1, [10, 20, 30], [3, 4, 5])
      _create_1d_z_nc(p2, [50, 60, 70], [6, 7, 8])

      with gdal.Run(
          "mdim",
          "mosaic",
          input=[p1, p2],
          output=os.path.join(tmpdir, "out.vrt"),
          array="test",
          output_format="VRT",
      ) as alg:
        ds = alg.Output()
        ar = ds.GetRootGroup().OpenMDArray("test")
        dims = ar.GetDimensions()
        self.assertEqual(dims[0].GetName(), "z")
        self.assertEqual(dims[0].GetType(), "VERTICAL")
        self.assertEqual(dims[0].GetDirection(), "UP")
        self.assertEqual(dims[0].GetSize(), 7)
        self.assertEqual(
            array.array("d", dims[0].GetIndexingVariable().Read()),
            array.array("d", [10, 20, 30, 40, 50, 60, 70]),
        )
        self.assertEqual(
            array.array("B", ar.Read()), array.array("B", [3, 4, 5, 0, 6, 7, 8])
        )

  def test_labelled_axis_2d(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_2d_z_dim1_nc(p1, [20], [100, 200, 300], [("test", [4, 5, 6])])
      _create_2d_z_dim1_nc(p2, [10], [100, 200, 300], [("test", [1, 2, 3])])

      out_nc = os.path.join(tmpdir, "out.nc")
      gdal.Run("mdim", "mosaic", input=[p1, p2], output=out_nc)
      with gdal.OpenEx(out_nc, gdal.OF_MULTIDIM_RASTER) as ds:
        ar = ds.GetRootGroup().OpenMDArray("test")
        dims = ar.GetDimensions()
        self.assertEqual(dims[0].GetName(), "z")
        self.assertEqual(dims[0].GetType(), "VERTICAL")
        self.assertEqual(dims[0].GetDirection(), "UP")
        self.assertEqual(dims[0].GetSize(), 2)
        self.assertEqual(
            array.array("d", dims[0].GetIndexingVariable().Read()),
            array.array("d", [10, 20]),
        )
        self.assertEqual(dims[1].GetName(), "dim1")
        self.assertEqual(dims[1].GetSize(), 3)
        self.assertEqual(
            array.array("d", dims[1].GetIndexingVariable().Read()),
            array.array("d", [100, 200, 300]),
        )
        self.assertEqual(
            array.array("B", ar.Read()), array.array("B", [1, 2, 3, 4, 5, 6])
        )

  @parameterized.parameters(
      (
          [10, 20, 30],
          [70, 60, 50],
          None,
          (
              "is indexed by a variable with spacing -10, whereas it is 10 in"
              " other datasets"
          ),
      ),
      (
          [10, 20, 30],
          [25, 35, 45],
          None,
          (
              "is indexed by a variable whose start value is not aligned with"
              " the one of other datasets"
          ),
      ),
      (
          [10, 20, 30],
          [35],
          None,
          "has irregularly-spaced values, contrary to other datasets",
      ),
      (
          [10],
          [30, 40, 50],
          None,
          "has regularly spaced labels, contrary to other datasets",
      ),
      (
          [10, 11, 20],
          [30, 29, 25],
          None,
          "must be either increasing or decreasing in all input datasets",
      ),
      (
          [10, 20, 40],
          [10, 20, 50],
          None,
          (
              "values in indexing variable z of dimension z are not the same as"
              " in other datasets"
          ),
      ),
      (
          [10, 20, 40],
          [5, 20, 50],
          None,
          (
              "values in indexing variable z of dimension z are not the same as"
              " in other datasets"
          ),
      ),
      (
          [10, 20, 40],
          [40, 50, 70],
          None,
          (
              "values in indexing variable z of dimension z are overlapping"
              " with the ones of other datasets"
          ),
      ),
      (
          [40, 50, 70],
          [100, 110, 130],
          [90, 105, 106],
          (
              "values in indexing variable z of dimension z are overlapping"
              " with the ones of other datasets"
          ),
      ),
  )
  def test_errors(
      self,
      values1: list[float],
      values2: list[float],
      values3: list[float] | None,
      error_msg: str,
  ) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_1d_z_nc(p1, values1, [1] * len(values1), with_z_attrs=False)
      _create_1d_z_nc(p2, values2, [2] * len(values2), with_z_attrs=False)
      inputs = [p1, p2]
      if values3 is not None:
        p3 = os.path.join(tmpdir, "test3.nc")
        _create_1d_z_nc(p3, values3, [3] * len(values3), with_z_attrs=False)
        inputs.append(p3)

      with self.assertRaisesRegex(RuntimeError, error_msg):
        gdal.Run(
            "mdim",
            "mosaic",
            input=inputs,
            output="",
            array="test",
            output_format="VRT",
        )

  def test_error_dim_not_same_name(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_1d_z_nc(p1, [1], [1], with_z_attrs=False, dim_name="z")
      _create_1d_z_nc(p2, [2], [2], with_z_attrs=False, dim_name="y")

      with self.assertRaisesRegex(
          RuntimeError, "does not have the same name as in other datasets"
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1, p2],
            output="",
            array="test",
            output_format="VRT",
        )

  def test_error_array_not_same_type(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_1d_z_nc(p1, [1], [1], with_z_attrs=False, dtype=gdal.GDT_Byte)
      _create_1d_z_nc(p2, [2], [2], with_z_attrs=False, dtype=gdal.GDT_Int32)

      with self.assertRaisesRegex(
          RuntimeError, "does not have the same data type as in other datasets"
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1, p2],
            output="",
            array="test",
            output_format="VRT",
        )

  @parameterized.parameters((1, 2), (None, 2), (1, None))
  def test_error_array_not_same_nodata_value(
      self, nd1: int | None, nd2: int | None
  ) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      p2 = os.path.join(tmpdir, "test2.nc")
      _create_1d_z_nc(p1, [1], [1], with_z_attrs=False, nodata=nd1)
      _create_1d_z_nc(p2, [2], [2], with_z_attrs=False, nodata=nd2)

      with self.assertRaisesRegex(
          RuntimeError,
          "does not have the same nodata value as in other datasets",
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1, p2],
            output="",
            array="test",
            output_format="VRT",
        )

  def test_error_non_existing_arrays(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      _create_1d_z_nc(p1, [1], [1], with_z_attrs=False)

      with self.assertRaisesRegex(
          RuntimeError, "No array of dimension count >= 2 found in dataset"
      ):
        gdal.Run("mdim", "mosaic", input=[p1], output="", output_format="VRT")

      with self.assertRaisesRegex(RuntimeError, "Cannot find array"):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1],
            output="",
            array="non_existing",
            output_format="VRT",
        )

  def test_error_zero_dim(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      with gdal.GetDriverByName("netCDF").CreateMultiDimensional(p1) as ds:
        rg = ds.GetRootGroup()
        ar = rg.CreateMDArray(
            "test", [], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )
        ar.Write(b"\x00")

      with self.assertRaisesRegex(RuntimeError, "has no dimension"):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1],
            output="",
            array="test",
            output_format="VRT",
        )

  def test_error_non_numeric_indexing_var(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.nc")
      with gdal.GetDriverByName("netCDF").CreateMultiDimensional(p1) as ds:
        rg = ds.GetRootGroup()
        z = rg.CreateDimension("z", "VERTICAL", "UP", 1)
        z_ar = rg.CreateMDArray("z", [z], gdal.ExtendedDataType.CreateString())
        z_ar.Write(["foo"])
        ar = rg.CreateMDArray(
            "test", [z], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )
        ar.Write(array.array("B", [1]))

      with self.assertRaisesRegex(
          RuntimeError,
          "indexing variable z of dimension z has a non-numeric data type",
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1],
            output="",
            array="test",
            output_format="VRT",
        )

  def test_error_no_indexing_var(self) -> None:
    if not utilities_util.has_driver("Zarr"):
      self.skipTest("Zarr driver not available")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      p1 = os.path.join(tmpdir, "test1.zarr")
      with gdal.GetDriverByName("Zarr").CreateMultiDimensional(p1) as ds:
        rg = ds.GetRootGroup()
        z = rg.CreateDimension("z", "VERTICAL", "UP", 1)
        ar = rg.CreateMDArray(
            "test", [z], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
        )
        ar.Write(array.array("B", [1]))

      with self.assertRaisesRegex(
          RuntimeError, "dimension z lacks an indexing variable"
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=[p1],
            output="",
            array="test",
            output_format="VRT",
        )

  def test_multiple_arrays(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      _create_2d_z_dim1_nc(
          os.path.join(tmpdir, "test1.nc"),
          [10],
          [100],
          [("test", [3]), ("test2", [30])],
      )
      _create_2d_z_dim1_nc(
          os.path.join(tmpdir, "test2.nc"),
          [20],
          [100],
          [("test", [4]), ("test2", [40])],
      )

      in_glob = os.path.join(tmpdir, "test*.nc")
      out_vrt = os.path.join(tmpdir, "out.vrt")

      with gdal.Run(
          "mdim", "mosaic", input=in_glob, output=out_vrt, output_format="VRT"
      ) as alg:
        ds = alg.Output()
        self.assertEqual(
            set(ds.GetRootGroup().GetMDArrayNames()),
            {"dim1", "test", "test2", "z"},
        )
        for name, expected in (("test", [3, 4]), ("test2", [30, 40])):
          ar = ds.GetRootGroup().OpenMDArray(name)
          dims = ar.GetDimensions()
          self.assertEqual(dims[0].GetName(), "z")
          self.assertEqual(dims[0].GetType(), "VERTICAL")
          self.assertEqual(dims[0].GetDirection(), "UP")
          self.assertEqual(dims[0].GetSize(), 2)
          self.assertEqual(
              array.array("d", dims[0].GetIndexingVariable().Read()),
              array.array("d", [10, 20]),
          )
          self.assertEqual(
              array.array("B", ar.Read()), array.array("B", expected)
          )

      with gdal.Run(
          "mdim",
          "mosaic",
          input=in_glob,
          output=out_vrt,
          output_format="VRT",
          array=["test", "test2"],
          overwrite=True,
      ) as alg:
        ds = alg.Output()
        self.assertEqual(
            set(ds.GetRootGroup().GetMDArrayNames()),
            {"dim1", "test", "test2", "z"},
        )

      with self.assertRaisesRegex(
          RuntimeError, "You may specify the --overwrite option"
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=in_glob,
            output=out_vrt,
            output_format="VRT",
            array=["test", "non_existing"],
        )

      with self.assertRaisesRegex(
          RuntimeError, "Cannot find array /non_existing"
      ):
        gdal.Run(
            "mdim",
            "mosaic",
            input=in_glob,
            output=out_vrt,
            output_format="VRT",
            overwrite=True,
            array=["test", "non_existing"],
        )

  def test_copy_blocksize(self) -> None:
    src_nc = utilities_util.get_testdata_path(
        "../gdrivers/data/netcdf/byte_chunked_not_multiple.nc"
    )
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      out_nc = os.path.join(tmpdir, "out.nc")
      with gdal.Run("mdim", "mosaic", input=src_nc, output=out_nc) as alg:
        ds = alg.Output()
        self.assertEqual(
            ds.GetRootGroup().OpenMDArray("Band1").GetBlockSize(), [6, 15]
        )

  def test_copy_blocksize_not_same(self) -> None:
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      _create_2d_z_dim1_nc(
          os.path.join(tmpdir, "test1.nc"),
          [10],
          [100, 200, 300],
          [("test", None)],
          creation_options=["BLOCKSIZE=1,2"],
      )
      _create_2d_z_dim1_nc(
          os.path.join(tmpdir, "test2.nc"),
          [20],
          [100, 200, 300],
          [("test", None)],
          creation_options=["BLOCKSIZE=1,1"],
      )

      out_nc = os.path.join(tmpdir, "out.nc")
      gdal.Run(
          "mdim",
          "mosaic",
          input=os.path.join(tmpdir, "test*.nc"),
          output=out_nc,
      )
      with gdal.OpenEx(out_nc, gdal.OF_MULTIDIM_RASTER) as ds:
        self.assertEqual(
            ds.GetRootGroup().OpenMDArray("test").GetBlockSize(), [0, 0]
        )

  def test_two_sources(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with gcore_util.TestTemporaryDirectory() as tmpdir:
      top_nc = os.path.join(tmpdir, "out_top.nc")
      bottom_nc = os.path.join(tmpdir, "out_bottom.nc")
      out_vrt = os.path.join(tmpdir, "out.vrt")

      gdal.Run(
          "raster clip", input=byte_tif, output=top_nc, window=[0, 0, 20, 10]
      )
      gdal.Run(
          "raster clip",
          input=byte_tif,
          output=bottom_nc,
          window=[0, 10, 20, 10],
      )

      gdal.Run("mdim mosaic", input=[top_nc, bottom_nc], output=out_vrt)
      with gdal.OpenEx(out_vrt, gdal.OF_MULTIDIM_RASTER) as ds:
        with (
            ds.GetRootGroup().OpenMDArray("Band1").AsClassicDataset(1, 0)
        ) as classic_ds:
          self.assertEqual(classic_ds.GetRasterBand(1).Checksum(), 4855)

      gdal.Run(
          "mdim mosaic",
          input=[bottom_nc, top_nc],
          output=out_vrt,
          overwrite=True,
      )
      with gdal.OpenEx(out_vrt, gdal.OF_MULTIDIM_RASTER) as ds:
        with (
            ds.GetRootGroup().OpenMDArray("Band1").AsClassicDataset(1, 0)
        ) as classic_ds:
          self.assertEqual(classic_ds.GetRasterBand(1).Checksum(), 4855)


if __name__ == "__main__":
  absltest.main()
